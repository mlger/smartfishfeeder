/**
 * @file main.cpp
 * @brief Catfish Feeder — Relay Unit firmware (ESP8266 NodeMCU V3, Arduino core 3.x).
 *
 * Hardware (docs/notes/hardware-4.3b.md, "Feeder actuation side"):
 *   - ESP8266 NodeMCU V3
 *   - Generic 5V 1-channel optocoupler relay module, input on D1 (GPIO5),
 *     ACTIVE-LOW by default (see RELAY_ACTIVE_LEVEL in config.h)
 *   - 5V DC feeder motor (auger/roller) switched through the relay NO contact
 *     in series with +5V; 1N4007 flyback diode across the motor terminals;
 *     common GND between relay unit and display unit.
 *
 * HTTP API (LAN-only by design; JSON unless noted):
 *   GET  /health                  -> 200 "OK" (no auth; ESP32 validates a discovered IP)
 *   GET  /status                  -> JSON telemetry (no auth)
 *   POST /feed                    -> body {"seconds":1..60, "token":"..."} (auth)
 *   GET  /feed?seconds=N&token=T  -> same logic, curl convenience (auth)
 *   GET  /                        -> minimal status page, 10 s auto-refresh
 *   POST /config                  -> {"token":"<current>", "maxPulseS":1..60,
 *                                     "pulseS":1..30, "newToken":"<optional>",
 *                                     "mechanism":"motor|servo|stepper",
 *                                     "servoRestDeg","servoFeedDeg","stepHz",
 *                                     "stepDir"} (auth; P4 keys optional)
 *
 * Feeding mechanisms (Phase 4; all keep "feed for N seconds" semantics):
 *   motor (default) D1 relay/FET ON for N s — original behaviour
 *   servo           D5 servo sweeps rest<->feed angle every 0.5 s for N s,
 *                   parks at rest after (attach/feed/detach per feed)
 *   stepper         D2 STEP pulses at stepHz for N s, D6 DIR held at the
 *                   configured level (A4988/DRV8825-style driver)
 * The mechanism + its parameters live in a second, ADDITIVE EEPROM record
 * (offset 64, own magic/checksum) so an upgrade never touches the original
 * token/pulse blob.
 *
 * Feed pulse rules:
 *   - hard cap MAX_PULSE_LIMIT_S (60 s) enforced at request parse AND engine start
 *   - per-device soft cap cfg.maxPulseS (default 30 s): larger requests are
 *     clamped and the response carries a "note"
 *   - busy guard: request while feeding -> 409 {"ok":false,"error":"busy"}
 *
 * Time: configTime("PHT-8", "pool.ntp.org", ...). lastFeedEpoch uses the real
 * NTP epoch once SNTP syncs, otherwise a millis()/1000 pseudo-epoch
 * (see ntpTimeSynced()/currentEpoch()); /status exposes "ntpSynced".
 *
 * OTA: ArduinoOTA is intentionally OPEN (no password) — this is a LAN-only
 * appliance; do not port-forward it. Documented decision (task 4-b).
 *
 * Token: DEFAULT_TOKEN "catfish" is the shipped product default (not a
 * secret); rotate on-site via POST /config. Never logged, never persisted
 * outside EEPROM.
 */

#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <Servo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

#include "config.h"

// ---------------------------------------------------------------------------
// Logging (Serial 115200, every line tagged [RELAY])
// ---------------------------------------------------------------------------
#define LOG(...)                            \
  do {                                      \
    Serial.print(F("[RELAY] "));            \
    Serial.printf(__VA_ARGS__);             \
    Serial.println();                       \
  } while (0)

// ---------------------------------------------------------------------------
// Persistent configuration (EEPROM)
// ---------------------------------------------------------------------------
struct __attribute__((packed)) RelayConfig {
  uint32_t magic;                 // CFG_MAGIC
  char token[RELAY_TOKEN_LEN];    // NUL-terminated feed/config token
  uint8_t maxPulseS;              // soft cap, seconds (1..MAX_PULSE_LIMIT_S)
  uint8_t pulseS;                 // default pulse, seconds (1..PULSE_S_LIMIT)
  uint32_t lastFeedEpoch;         // epoch of last successful feed (0 = never)
  uint32_t checksum;              // FNV-1a over everything above this field
};
static_assert(sizeof(RelayConfig) == 38, "RelayConfig EEPROM layout changed");

static RelayConfig g_cfg;

static uint32_t fnv1a(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t h = 2166136261u;
  while (len--) {
    h ^= *p++;
    h *= 16777619u;
  }
  return h;
}

static uint32_t cfgChecksum(const RelayConfig& c) {
  return fnv1a(&c, offsetof(RelayConfig, checksum));
}

static void configDefaults(RelayConfig& c) {
  memset(&c, 0, sizeof(c));
  c.magic = CFG_MAGIC;
  snprintf(c.token, sizeof(c.token), "%s", DEFAULT_TOKEN);
  c.maxPulseS = DEFAULT_MAX_PULSE_S;
  c.pulseS = DEFAULT_PULSE_S;
  c.lastFeedEpoch = 0;
  c.checksum = cfgChecksum(c);
}

static void saveConfig() {
  g_cfg.checksum = cfgChecksum(g_cfg);
  EEPROM.put(0, g_cfg);
  EEPROM.commit();  // only rewrites flash when bytes actually changed
}

static void configLoad() {
  EEPROM.begin(EEPROM_SIZE);
  RelayConfig c;
  EEPROM.get(0, c);
  const bool valid = (c.magic == CFG_MAGIC) && (c.checksum == cfgChecksum(c));
  if (!valid) {
    LOG("EEPROM empty/invalid -> writing defaults");
    configDefaults(c);
    g_cfg = c;
    saveConfig();
    return;
  }
  // Belt and braces: never trust flash-resident strings / ranges.
  c.token[RELAY_TOKEN_LEN - 1] = '\0';
  if (c.maxPulseS < 1 || c.maxPulseS > MAX_PULSE_LIMIT_S) c.maxPulseS = DEFAULT_MAX_PULSE_S;
  if (c.pulseS < 1 || c.pulseS > PULSE_S_LIMIT) c.pulseS = DEFAULT_PULSE_S;
  if (c.pulseS > c.maxPulseS) c.pulseS = c.maxPulseS;
  g_cfg = c;
  LOG("config loaded: maxPulseS=%u pulseS=%u lastFeedEpoch=%u", (unsigned)g_cfg.maxPulseS,
      (unsigned)g_cfg.pulseS, (unsigned)g_cfg.lastFeedEpoch);
}

// ---------------------------------------------------------------------------
// Mechanism config — second, ADDITIVE EEPROM record (Phase 4). Lives at
// MECH_EEPROM_OFFSET with its own magic/checksum, so flashing this firmware
// onto an existing install preserves the original token/pulse blob at
// offset 0 byte-for-byte and only writes defaults into the new record.
// ---------------------------------------------------------------------------
struct __attribute__((packed)) MechanismConfig {
  uint32_t magic;        // MECH_MAGIC
  uint8_t mechanism;     // CATFISH_MECH_* (0=motor 1=servo 2=stepper)
  uint8_t servoRestDeg;  // idle angle 0..180
  uint8_t servoFeedDeg;  // feed angle 0..180
  uint16_t stepHz;       // stepper step rate, steps/s (1..1000)
  uint8_t stepDir;       // DIR level during feed (0/1)
  uint32_t checksum;     // FNV-1a over everything above this field
};
static_assert(sizeof(MechanismConfig) == 14, "MechanismConfig EEPROM layout changed");

static MechanismConfig g_mech;
static Servo g_servo;

static uint32_t mechChecksum(const MechanismConfig &m) {
  return fnv1a(&m, offsetof(MechanismConfig, checksum));
}

static void mechDefaults(MechanismConfig &m) {
  memset(&m, 0, sizeof(m));
  m.magic = MECH_MAGIC;
  m.mechanism = CATFISH_MECH_DEFAULT;
  m.servoRestDeg = CATFISH_SERVO_REST_DEFAULT;
  m.servoFeedDeg = CATFISH_SERVO_FEED_DEFAULT;
  m.stepHz = CATFISH_STEP_HZ_DEFAULT;
  m.stepDir = 0;
  m.checksum = mechChecksum(m);
}

static void saveMechConfig() {
  g_mech.checksum = mechChecksum(g_mech);
  EEPROM.put(MECH_EEPROM_OFFSET, g_mech);
  EEPROM.commit();  // only rewrites flash when bytes actually changed
}

static void mechConfigLoad() {
  MechanismConfig m;
  EEPROM.get(MECH_EEPROM_OFFSET, m);
  if ((m.magic != MECH_MAGIC) || (m.checksum != mechChecksum(m))) {
    LOG("mech EEPROM empty/invalid -> writing defaults (motor)");
    mechDefaults(m);
    g_mech = m;
    saveMechConfig();
    return;
  }
  // Belt and braces: never trust flash-resident ranges.
  if (m.mechanism >= CATFISH_MECH_COUNT) m.mechanism = CATFISH_MECH_DEFAULT;
  m.servoRestDeg = catfishClampServoDeg(m.servoRestDeg);
  m.servoFeedDeg = catfishClampServoDeg(m.servoFeedDeg);
  m.stepHz = catfishClampStepHz(m.stepHz);
  m.stepDir = catfishClampStepDir(m.stepDir);
  g_mech = m;
  LOG("mech loaded: %s rest=%u feed=%u hz=%u dir=%u", catfishMechName(g_mech.mechanism),
      (unsigned)g_mech.servoRestDeg, (unsigned)g_mech.servoFeedDeg,
      (unsigned)g_mech.stepHz, (unsigned)g_mech.stepDir);
}

/** STEP/DIR idle-low from the first microseconds (same discipline as RELAY_PIN). */
static void mechPinsBootSafe() {
  digitalWrite(STEP_PIN, LOW);
  pinMode(STEP_PIN, OUTPUT);
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, LOW);
  pinMode(DIR_PIN, OUTPUT);
  digitalWrite(DIR_PIN, LOW);
}

// ---------------------------------------------------------------------------
// Feed token — constant-time-ish compare (only the length leaks, acceptable
// for a LAN appliance per task spec).
// ---------------------------------------------------------------------------
static bool tokenMatches(const char* provided) {
  if (provided == nullptr) return false;
  const size_t plen = strlen(provided);
  const size_t slen = strnlen(g_cfg.token, RELAY_TOKEN_LEN);
  if (plen != slen) return false;
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < slen; ++i) diff |= (uint8_t)(provided[i] ^ g_cfg.token[i]);
  return diff == 0;
}

// ---------------------------------------------------------------------------
// Time: NTP (UTC+8) with millis() pseudo-epoch fallback
// ---------------------------------------------------------------------------
static bool ntpTimeSynced() {
  // ~Sep 2020: SNTP has set a sane wall clock (not the 1970 default).
  return (uint32_t)time(nullptr) > 1600000000UL;
}

// "now-ish": real epoch once NTP syncs, otherwise seconds-since-boot.
static uint32_t currentEpoch() {
  if (ntpTimeSynced()) return (uint32_t)time(nullptr);
  return (uint32_t)(millis() / 1000UL);
}

// ---------------------------------------------------------------------------
// Status LED: heartbeat 2 s / fast-blink while feeding / solid (config portal)
// ---------------------------------------------------------------------------
enum LedMode : uint8_t {
  LED_MODE_HEARTBEAT = 0,
  LED_MODE_FEED,
  LED_MODE_SOLID,
};

static uint8_t g_ledMode = LED_MODE_HEARTBEAT;

static void statusLedTick() {
  const uint32_t now = millis();
  switch (g_ledMode) {
    case LED_MODE_SOLID:
      digitalWrite(STATUS_LED, LED_ON);
      break;
    case LED_MODE_FEED:
      digitalWrite(STATUS_LED, ((now / LED_FEED_BLINK_MS) & 1u) ? LED_OFF : LED_ON);
      break;
    case LED_MODE_HEARTBEAT:
    default:
      digitalWrite(STATUS_LED,
                   (now % LED_HEARTBEAT_PERIOD_MS) < LED_HEARTBEAT_ON_MS ? LED_ON : LED_OFF);
      break;
  }
}

// ---------------------------------------------------------------------------
// Pulse engine — non-blocking, millis()/micros()-based, mechanism-aware.
// "Feed for N seconds" semantics are identical for all mechanisms; only the
// pin behaviour during those seconds differs (catfish_mech.h).
// ---------------------------------------------------------------------------
static bool g_feeding = false;
static uint32_t g_feedStartMs = 0;
static uint32_t g_feedDurMs = 0;

// servo sweep phase + stepper edge timing (engine-local, reset on each start)
static uint32_t g_mech_phase_ms = 0;
static bool g_servo_at_rest = true;
static uint32_t g_last_step_edge_us = 0;
static bool g_step_level = false;

/** Attach + drive to the rest angle, let it arrive, detach (holds by detent). */
static void mechServoPark() {
  g_servo.attach(SERVO_PIN);
  g_servo.write(g_mech.servoRestDeg);
  delay(400);  // setup/config context only — never called from loop()
  g_servo.detach();
}

static void startFeedPulse(uint32_t seconds) {
  if (seconds < 1) seconds = 1;  // paranoia
  if (seconds > (uint32_t)MAX_PULSE_LIMIT_S) seconds = (uint32_t)MAX_PULSE_LIMIT_S;  // hard cap
  g_feedStartMs = millis();
  g_feedDurMs = seconds * 1000UL;
  g_cfg.lastFeedEpoch = currentEpoch();
  g_feeding = true;
  g_ledMode = LED_MODE_FEED;
  switch (g_mech.mechanism) {
    case CATFISH_MECH_SERVO:
      g_servo.attach(SERVO_PIN);
      g_servo.write(g_mech.servoRestDeg);  // start from rest, sweep below
      g_mech_phase_ms = millis();
      g_servo_at_rest = true;
      break;
    case CATFISH_MECH_STEPPER:
      digitalWrite(DIR_PIN, g_mech.stepDir ? HIGH : LOW);
      g_last_step_edge_us = micros();
      g_step_level = false;
      break;
    case CATFISH_MECH_MOTOR:
    default:
      digitalWrite(RELAY_PIN, RELAY_ON);
      break;
  }
  saveConfig();  // persist lastFeedEpoch even if power is cut mid-pulse
  LOG("feed START %u s (%s)", (unsigned)seconds, catfishMechName(g_mech.mechanism));
}

static void stopFeedPulse() {
  // v1.8.2: idempotent ALL-OFF — this used to switch on g_mech.mechanism at
  // stop time, so a mechanism change DURING a feed (e.g. motor -> servo via
  // /config) skipped the RELAY off path and latched the motor ON forever.
  digitalWrite(RELAY_PIN, RELAY_OFF);
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, LOW);
  g_servo.write(g_mech.servoRestDeg);
  g_servo.detach();  // cheap servos hold by detent; stops idle hum
  g_feeding = false;
  g_ledMode = LED_MODE_HEARTBEAT;
  LOG("feed END (%s idle)", catfishMechName(g_mech.mechanism));
}

/** Per-millisecond mechanism behaviour while a feed is running. */
static void mechTick() {
  if (!g_feeding) return;
  switch (g_mech.mechanism) {
    case CATFISH_MECH_SERVO:
      // sweep rest <-> feed every 500 ms (1 Hz full cycle)
      if (millis() - g_mech_phase_ms >= 500) {
        g_mech_phase_ms = millis();
        g_servo_at_rest = !g_servo_at_rest;
        g_servo.write(g_servo_at_rest ? g_mech.servoRestDeg : g_mech.servoFeedDeg);
      }
      break;
    case CATFISH_MECH_STEPPER: {
      // toggle STEP every half period (A4988 needs >= 1.9 us per level; at
      // stepHz = 1 kHz that is 500 us — comfortable for a loop() poll)
      const uint32_t half_us = 500000UL / g_mech.stepHz;
      const uint32_t now_us = micros();
      if (now_us - g_last_step_edge_us >= half_us) {
        g_last_step_edge_us = now_us;
        g_step_level = !g_step_level;
        digitalWrite(STEP_PIN, g_step_level ? HIGH : LOW);
      }
      break;
    }
    default:
      break;  // motor: the on/off pulse IS the whole behaviour
  }
}

static void pulseEngineTick() {
  if (!g_feeding) return;
  if (millis() - g_feedStartMs >= g_feedDurMs) stopFeedPulse();  // rollover-safe subtraction
}

// ---------------------------------------------------------------------------
// UDP discovery responder (display unit finds this relay on the LAN)
// ---------------------------------------------------------------------------
static WiFiUDP g_discoveryUdp;

static void discoveryPump() {
  const int size = g_discoveryUdp.parsePacket();
  if (size <= 0) return;
  char pkt[64];
  const int nRaw = g_discoveryUdp.read(pkt, sizeof(pkt) - 1);
  if (nRaw <= 0) return;
  int n = nRaw;
  pkt[n] = '\0';
  // v1.10.0 FIX: tolerate trailing whitespace/newline. The display unit
  // broadcasts "CATFISH_DISCOVER\n"; the old exact strcmp never matched, so
  // UDP discovery (and multi-tank auto-binding) was dead between shipped
  // firmwares. Trim, then compare.
  while (n > 0 && (pkt[n - 1] == '\n' || pkt[n - 1] == '\r' ||
                   pkt[n - 1] == ' ' || pkt[n - 1] == '\t')) {
    pkt[--n] = '\0';
  }
  if (strcmp(pkt, DISCOVERY_REQ) != 0) return; // exact match, per protocol

  const IPAddress rip = g_discoveryUdp.remoteIP();
  const uint16_t rport = g_discoveryUdp.remotePort();
  char rsp[40];  // "CATFISH_RELAY|" (14) + dotted quad (15) + NUL
  snprintf(rsp, sizeof(rsp), "%s%s", DISCOVERY_RSP_PREFIX, WiFi.localIP().toString().c_str());
  g_discoveryUdp.beginPacket(rip, rport);
  g_discoveryUdp.print(rsp);
  g_discoveryUdp.endPacket();
  LOG("discovery: %s:%u -> %s", rip.toString().c_str(), (unsigned)rport, rsp);
}

// ---------------------------------------------------------------------------
// WiFi reconnect watchdog
// ---------------------------------------------------------------------------
static uint32_t g_wifiLastOkMs = 0;
static bool g_wifiUp = false;

static void wifiWatchdog() {
  const bool up = (WiFi.status() == WL_CONNECTED);
  if (up) {
    if (!g_wifiUp) {
      g_wifiUp = true;
      LOG("wifi up: ip=%s rssi=%d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    }
    g_wifiLastOkMs = millis();
    return;
  }
  if (g_wifiUp) {
    g_wifiUp = false;
    LOG("wifi lost, watchdog armed");
  }
  if (millis() - g_wifiLastOkMs >= WIFI_RECONNECT_AFTER_MS) {
    LOG("wifi down > %u s -> WiFi.reconnect()", (unsigned)(WIFI_RECONNECT_AFTER_MS / 1000UL));
    WiFi.reconnect();
    g_wifiLastOkMs = millis();  // retry at the next interval, not in a tight loop
  }
}

// ---------------------------------------------------------------------------
// Factory reset: hold the FLASH button (GPIO0) for FACTORY_RESET_HOLD_MS to
// wipe the EEPROM config back to defaults and reboot. Refused while a feed
// pulse is running (restart while the relay is energized would glitch the
// motor line; the wipe also persists lastFeedEpoch=0 which we do not want
// to record mid-pulse).
// ---------------------------------------------------------------------------
static bool s_resetArmed = false;
static uint32_t s_resetStartMs = 0;

static void factoryResetTick() {
  if (digitalRead(FACTORY_RESET_BTN) != LOW) {
    if (s_resetArmed && !g_feeding) g_ledMode = LED_MODE_HEARTBEAT;
    s_resetArmed = false;
    return;
  }
  if (!s_resetArmed) {
    s_resetArmed = true;
    s_resetStartMs = millis();
    if (!g_feeding) g_ledMode = LED_MODE_SOLID;  // visual "armed" feedback
    return;
  }
  if (g_feeding) return;  // never wipe mid-pulse
  if (millis() - s_resetStartMs >= FACTORY_RESET_HOLD_MS) {
    LOG("factory reset: FLASH held %u s - wiping config to defaults",
        (unsigned)(FACTORY_RESET_HOLD_MS / 1000UL));
    configDefaults(g_cfg);
    saveConfig();
    mechDefaults(g_mech);  // P4: mechanism record resets too (servo re-parks below)
    saveMechConfig();
    if (g_mech.mechanism == CATFISH_MECH_SERVO && !g_feeding) mechServoPark();
    g_ledMode = LED_MODE_FEED;  // fast blink as confirmation
    delay(1500);
    ESP.restart();
  }
}

// ---------------------------------------------------------------------------
// HTTP server (ESP8266WebServer, port 80)
// ---------------------------------------------------------------------------
static ESP8266WebServer g_server(80);

static void sendJsonError(int code, const char* err) {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", err);
  g_server.send(code, "application/json", buf);
}

static void handleHealth() { g_server.send(200, "text/plain", "OK"); }

static void handleStatus() {
  const uint32_t nowMs = millis();
  uint32_t secondsLeft = 0;
  if (g_feeding) {
    const uint32_t elapsed = nowMs - g_feedStartMs;
    const uint32_t remainingMs = (elapsed >= g_feedDurMs) ? 0 : (g_feedDurMs - elapsed);
    secondsLeft = (remainingMs + 999UL) / 1000UL;  // ceil
  }

  JsonDocument doc;  // ArduinoJson v7 (heap-pooled)
  doc["ok"] = true;
  doc["state"] = g_feeding ? "feeding" : "ready";
  doc["feeding"] = g_feeding;
  doc["secondsLeft"] = (int)secondsLeft;
  doc["lastFeedEpoch"] = g_cfg.lastFeedEpoch;
  doc["pulseS"] = (int)g_cfg.pulseS;
  doc["maxPulseS"] = (int)g_cfg.maxPulseS;
  doc["uptimeS"] = nowMs / 1000UL;
  doc["heap"] = ESP.getFreeHeap();
  doc["rssi"] = (int)WiFi.RSSI();
  doc["ip"] = WiFi.localIP().toString();
  doc["version"] = FIRMWARE_VERSION;
  doc["mechanism"] = catfishMechName(g_mech.mechanism);
  doc["servoRestDeg"] = (int)g_mech.servoRestDeg;
  doc["servoFeedDeg"] = (int)g_mech.servoFeedDeg;
  doc["stepHz"] = (int)g_mech.stepHz;
  // Wire-contract revision (shared header). The display parses "proto" and
  // warns when the two firmwares disagree; bump CATFISH_PROTOCOL_VERSION in
  // ../common/catfish_protocol.h whenever the JSON/endpoint contract changes.
  doc["proto"] = CATFISH_PROTOCOL_VERSION;
  doc["ntpSynced"] = ntpTimeSynced();  // extra field: explains lastFeedEpoch semantics

  char buf[640];
  const size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n == 0) {
    sendJsonError(500, "serialize_failed");
    return;
  }
  g_server.send(200, "application/json", buf);
}

static void respondFeedSuccess(long seconds, bool clampedToCfg) {
  char buf[160];
  if (clampedToCfg) {
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"seconds\":%ld,\"note\":\"clamped to maxPulseS (%u)\"}",
             seconds, (unsigned)g_cfg.maxPulseS);
  } else {
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"seconds\":%ld}", seconds);
  }
  g_server.send(200, "application/json", buf);
}

// Shared by POST /feed and GET /feed. Order: auth -> validate -> clamp -> busy.
static void processFeedRequest(long seconds, const char* token) {
  if (!tokenMatches(token)) {
    sendJsonError(403, "auth");
    return;
  }
  if (seconds < 1) {
    sendJsonError(400, "bad_seconds");
    return;
  }
  if (seconds > (long)MAX_PULSE_LIMIT_S) seconds = (long)MAX_PULSE_LIMIT_S;  // hard-cap clamp
  bool clampedToCfg = false;
  if (seconds > (long)g_cfg.maxPulseS) {
    seconds = (long)g_cfg.maxPulseS;
    clampedToCfg = true;
  }
  if (g_feeding) {
    sendJsonError(409, "busy");
    return;
  }
  startFeedPulse((uint32_t)seconds);
  respondFeedSuccess(seconds, clampedToCfg);
}

static void handleFeedPost() {
  const String body = g_server.arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    sendJsonError(400, "bad_json");
    return;
  }
  // Strict integer check: "3.5", "3" (string), missing -> 400.
  if (!doc["seconds"].is<long>()) {
    sendJsonError(400, "bad_seconds");
    return;
  }
  const long seconds = doc["seconds"].as<long>();
  const char* token = doc["token"] | "";  // points into doc pool, valid for this call
  processFeedRequest(seconds, token);
}

static void handleFeedGet() {
  if (!g_server.hasArg("token")) {
    sendJsonError(403, "auth");
    return;
  }
  if (!g_server.hasArg("seconds")) {
    sendJsonError(400, "bad_seconds");
    return;
  }
  // Keep Strings alive on this stack frame (c_str() must stay valid through
  // processFeedRequest, which sends the response).
  const String tokenArg = g_server.arg("token");
  const String secondsArg = g_server.arg("seconds");
  char* end = nullptr;
  const long seconds = strtol(secondsArg.c_str(), &end, 10);
  if (end == secondsArg.c_str() || *end != '\0') {
    sendJsonError(400, "bad_seconds");
    return;
  }
  processFeedRequest(seconds, tokenArg.c_str());
}

static void handleRoot() {
  char page[896];  // literals ~430 B + values; snprintf truncates safely anyway
  const char* state = g_feeding ? "FEEDING" : "ready";
  char lastFeed[24];
  snprintf(lastFeed, sizeof(lastFeed), "%u", (unsigned)g_cfg.lastFeedEpoch);
  snprintf(page, sizeof(page),
           "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta http-equiv=\"refresh\" content=\"10\">"
           "<title>Catfish Feeder Relay</title></head>"
           "<body><h2>Catfish Feeder &mdash; Relay Unit</h2>"
           "<p>State: <b>%s</b></p>"
           "<p>Mechanism: <b>%s</b>%s</p>"
           "<p>Last feed epoch: %s%s</p>"
           "<p>IP: %s</p>"
           "<p>Firmware: %s | Uptime: %u s | Free heap: %u B</p>"
           "<p>API: GET /health, GET /status, POST/GET /feed, POST /config</p>"
           "</body></html>",
           state, catfishMechName(g_mech.mechanism),
           g_feeding ? "" : " (idle)",
           lastFeed, ntpTimeSynced() ? " (NTP)" : " (boot-seconds, NTP not synced)",
           WiFi.localIP().toString().c_str(), FIRMWARE_VERSION, (unsigned)(millis() / 1000UL),
           (unsigned)ESP.getFreeHeap());
  g_server.send(200, "text/html", page);
}

static void handleConfig() {
  const String body = g_server.arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    sendJsonError(400, "bad_json");
    return;
  }

  const char* token = doc["token"] | "";  // current token == auth credential
  if (!tokenMatches(token)) {
    sendJsonError(403, "auth");
    return;
  }

  // Validate the mechanism name BEFORE any mutation so a bad body never
  // leaves the config half-applied (mirrors the maxPulseS/pulseS contract).
  if (doc["mechanism"].is<const char*>() &&
      catfishMechFromName(doc["mechanism"].as<const char*>()) == 0xFF) {
    sendJsonError(400, "bad_mechanism");
    return;
  }
  // v1.8.2: reject a mechanism CHANGE while a feed is running. Previously a
  // switch mid-pulse made stopFeedPulse() take the NEW mechanism's stop path,
  // leaving the motor relay latched ON until reboot.
  if (g_feeding && doc["mechanism"].is<const char*>() &&
      catfishMechFromName(doc["mechanism"].as<const char*>()) != g_mech.mechanism) {
    sendJsonError(409, "busy_feeding");
    return;
  }

  bool changed = false;

  // v1.8.2: validate EVERYTHING into locals first, mutate only after all
  // checks pass. Previously newToken was written into g_cfg before the
  // maxPulseS/pulseS validation, so a 400 response kept the RAM token while
  // EEPROM still held the old one — the display then sent a token the relay
  // rejected (and reboot was the only cure).
  const char* newTok = nullptr;
  if (doc["newToken"].is<const char*>()) {
    const char* nt = doc["newToken"].as<const char*>();
    if (nt != nullptr && nt[0] != '\0') {
      if (strlen(nt) >= RELAY_TOKEN_LEN) {
        sendJsonError(400, "token_too_long");
        return;
      }
      newTok = nt;
    }
  }
  bool hasMax = false;
  uint8_t newMax = 0;
  if (doc["maxPulseS"].is<long>()) {
    const long v = doc["maxPulseS"].as<long>();
    if (v < 1 || v > (long)MAX_PULSE_LIMIT_S) {
      sendJsonError(400, "maxPulseS_range_1_60");
      return;
    }
    newMax = (uint8_t)v;
    hasMax = true;
  }
  bool hasPulse = false;
  uint8_t newPulse = 0;
  if (doc["pulseS"].is<long>()) {
    long v = doc["pulseS"].as<long>();
    if (v < 1 || v > (long)PULSE_S_LIMIT) {
      sendJsonError(400, "pulseS_range_1_30");
      return;
    }
    const uint8_t effMax = hasMax ? newMax : g_cfg.maxPulseS;
    if (v > (long)effMax) v = (long)effMax;  // keep pulseS <= maxPulseS
    newPulse = (uint8_t)v;
    hasPulse = true;
  }

  // ---- all validation passed: apply ----
  if (newTok != nullptr) {
    snprintf(g_cfg.token, sizeof(g_cfg.token), "%s", newTok);
    changed = true;
  }
  if (hasMax) {
    g_cfg.maxPulseS = newMax;
    changed = true;
  }
  if (hasPulse) {
    g_cfg.pulseS = newPulse;
    changed = true;
  }

  // ---- Phase 4: mechanism keys (all optional; clamped via shared helpers) --
  bool mechChanged = false;
  if (doc["mechanism"].is<const char*>()) {
    const uint8_t id = catfishMechFromName(doc["mechanism"].as<const char*>());
    if (id != g_mech.mechanism) {
      g_mech.mechanism = id;
      mechChanged = true;
    }
  }
  if (doc["servoRestDeg"].is<long>()) {
    const uint8_t d = catfishClampServoDeg(doc["servoRestDeg"].as<long>());
    if (d != g_mech.servoRestDeg) { g_mech.servoRestDeg = d; mechChanged = true; }
  }
  if (doc["servoFeedDeg"].is<long>()) {
    const uint8_t d = catfishClampServoDeg(doc["servoFeedDeg"].as<long>());
    if (d != g_mech.servoFeedDeg) { g_mech.servoFeedDeg = d; mechChanged = true; }
  }
  if (doc["stepHz"].is<long>()) {
    const uint16_t hz = catfishClampStepHz(doc["stepHz"].as<long>());
    if (hz != g_mech.stepHz) { g_mech.stepHz = hz; mechChanged = true; }
  }
  if (doc["stepDir"].is<long>()) {
    const uint8_t d = catfishClampStepDir(doc["stepDir"].as<long>());
    if (d != g_mech.stepDir) { g_mech.stepDir = d; mechChanged = true; }
  }
  if (mechChanged) {
    // apply immediately: (re)park the servo when switching to/retuning servo
    if (g_mech.mechanism == CATFISH_MECH_SERVO && !g_feeding) mechServoPark();
    saveMechConfig();
    LOG("mech config: %s rest=%u feed=%u hz=%u dir=%u", catfishMechName(g_mech.mechanism),
        (unsigned)g_mech.servoRestDeg, (unsigned)g_mech.servoFeedDeg,
        (unsigned)g_mech.stepHz, (unsigned)g_mech.stepDir);
  }

  if (changed) saveConfig();

  // Echo the effective config back to the authenticated caller (LAN convenience).
  JsonDocument echo;
  echo["ok"] = true;
  echo["changed"] = (changed || mechChanged);
  echo["token"] = g_cfg.token;
  echo["maxPulseS"] = (int)g_cfg.maxPulseS;
  echo["pulseS"] = (int)g_cfg.pulseS;
  echo["mechanism"] = catfishMechName(g_mech.mechanism);
  echo["servoRestDeg"] = (int)g_mech.servoRestDeg;
  echo["servoFeedDeg"] = (int)g_mech.servoFeedDeg;
  echo["stepHz"] = (int)g_mech.stepHz;
  char buf[320];
  const size_t n = serializeJson(echo, buf, sizeof(buf));
  if (n == 0) {
    sendJsonError(500, "serialize_failed");
    return;
  }
  g_server.send(200, "application/json", buf);
}

static void handleNotFound() { sendJsonError(404, "not_found"); }

static void registerRoutes() {
  g_server.on("/health", HTTP_GET, handleHealth);
  g_server.on("/status", HTTP_GET, handleStatus);
  g_server.on("/feed", HTTP_POST, handleFeedPost);
  g_server.on("/feed", HTTP_GET, handleFeedGet);
  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/config", HTTP_POST, handleConfig);
  g_server.onNotFound(handleNotFound);
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  // ---- BOOT SAFETY FIRST — before Serial, WiFi, everything. ----
  // The relay pin must never float LOW (active-LOW modules energize on LOW).
  // INPUT_PULLUP holds the line HIGH through the boot glitch window; we then
  // pre-load the output latch with the INACTIVE level while the pin is still
  // an input, and only afterwards switch to OUTPUT. Result: the coil stays
  // OFF from the first microseconds, even on active-LOW opto modules.
  pinMode(RELAY_PIN, INPUT_PULLUP);
  delay(1);
  digitalWrite(RELAY_PIN, RELAY_OFF);  // pre-load latch while still an input
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);  // drive INACTIVE for real

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LED_OFF);
  pinMode(FACTORY_RESET_BTN, INPUT_PULLUP);  // FLASH button (factory reset)
  mechPinsBootSafe();  // STEP/DIR low before anything else (P4)

  Serial.begin(115200);
  LOG("boot v%s (relay D1 active-%s, LED D4) -- outputs forced INACTIVE at boot",
      FIRMWARE_VERSION, (RELAY_ACTIVE_LEVEL == LOW) ? "LOW" : "HIGH");

  configLoad();
  mechConfigLoad();  // additive record at EEPROM offset 64 (P4)
  if (g_mech.mechanism == CATFISH_MECH_SERVO) {
    mechServoPark();  // servo starts parked at its rest angle
    LOG("servo parked at %u deg", (unsigned)g_mech.servoRestDeg);
  }

  // Relay must respond fast -> no WiFi power-save latency, auto-reconnect on.
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  WiFiManager wm;
  wm.setConfigPortalTimeout(WM_PORTAL_TIMEOUT_S);
  wm.setAPCallback([](WiFiManager* w) {
    (void)w;
    // loop() is blocked inside the (blocking) autoConnect(); show the portal
    // state by holding the LED solid ON until the portal resolves.
    g_ledMode = LED_MODE_SOLID;
    digitalWrite(STATUS_LED, LED_ON);
    LOG("config portal open (AP '%s') -- LED solid ON", AP_NAME);
  });
  LOG("wifi: autoConnect (AP fallback '%s')", AP_NAME);
  if (!wm.autoConnect(AP_NAME)) {
    LOG("wifi connect/portal failed (timeout %u s) -- restarting",
        (unsigned)WM_PORTAL_TIMEOUT_S);
    delay(1000);
    ESP.restart();
  }
  WiFi.hostname(HOSTNAME);  // re-assert after association (cheap, harmless)
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  g_ledMode = LED_MODE_HEARTBEAT;

  configTime(NTP_TZ, NTP_SERVER_1, NTP_SERVER_2);  // "PHT-8" = UTC+8 (POSIX sign inverted)

  // mDNS on ESP8266: LEAmDNS answers queries only after probing completes,
  // and probing advances ONLY inside MDNS.update(). Today ArduinoOTA.handle()
  // happens to pump it; loop() calls it explicitly too so mDNS never silently
  // dies if OTA is ever removed or gated (v1.8.2).
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    LOG("mDNS: http://%s.local", HOSTNAME);
  } else {
    LOG("mDNS begin FAILED (continuing without .local name)");
  }

  ArduinoOTA.setHostname(HOSTNAME);
  if (OTA_PASSWORD[0] != '\0') {
    ArduinoOTA.setPassword(OTA_PASSWORD);
    LOG("OTA ready (password protected)");
  } else {
    LOG("OTA ready (LAN, no password)");
  }
  ArduinoOTA.begin();

  if (g_discoveryUdp.begin(DISCOVERY_UDP_PORT)) {
    LOG("discovery responder on UDP %u (awaiting '%s')", (unsigned)DISCOVERY_UDP_PORT,
        DISCOVERY_REQ);
  } else {
    LOG("discovery UDP bind FAILED");
  }

  registerRoutes();
  g_server.begin();
  LOG("HTTP ready on :80 (health/status/feed/config)");

  g_wifiUp = (WiFi.status() == WL_CONNECTED);
  g_wifiLastOkMs = millis();
  LOG("ready v%s -- heap=%u", FIRMWARE_VERSION, (unsigned)ESP.getFreeHeap());
}

void loop() {
  pulseEngineTick();   // FIRST: outputs must release on time even under network load
  mechTick();          // P4: servo sweep / stepper pulses while feeding
  statusLedTick();
  factoryResetTick();  // FLASH button held -> wipe config + reboot
  MDNS.update();       // v1.8.2: explicit pump (see setup comment)
  g_server.handleClient();
  ArduinoOTA.handle();
  discoveryPump();
  wifiWatchdog();
  yield();  // ESP8266 SDK / WiFi stack housekeeping
}
