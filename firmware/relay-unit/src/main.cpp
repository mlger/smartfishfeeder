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
 *                                     "pulseS":1..30, "newToken":"<optional>"} (auth)
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
// Pulse engine — non-blocking, millis()-based
// ---------------------------------------------------------------------------
static bool g_feeding = false;
static uint32_t g_feedStartMs = 0;
static uint32_t g_feedDurMs = 0;

static void startFeedPulse(uint32_t seconds) {
  if (seconds < 1) seconds = 1;  // paranoia
  if (seconds > (uint32_t)MAX_PULSE_LIMIT_S) seconds = (uint32_t)MAX_PULSE_LIMIT_S;  // hard cap
  g_feedStartMs = millis();
  g_feedDurMs = seconds * 1000UL;
  g_cfg.lastFeedEpoch = currentEpoch();
  g_feeding = true;
  g_ledMode = LED_MODE_FEED;
  digitalWrite(RELAY_PIN, RELAY_ON);
  saveConfig();  // persist lastFeedEpoch even if power is cut mid-pulse
  LOG("feed START %u s (relay ON)", (unsigned)seconds);
}

static void stopFeedPulse() {
  digitalWrite(RELAY_PIN, RELAY_OFF);
  g_feeding = false;
  g_ledMode = LED_MODE_HEARTBEAT;
  LOG("feed END (relay OFF)");
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
  const int n = g_discoveryUdp.read(pkt, sizeof(pkt) - 1);
  if (n <= 0) return;
  pkt[n] = '\0';
  if (strcmp(pkt, DISCOVERY_REQ) != 0) return;  // exact match, per protocol

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
  doc["ntpSynced"] = ntpTimeSynced();  // extra field: explains lastFeedEpoch semantics

  char buf[512];
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
  char page[640];
  const char* state = g_feeding ? "FEEDING" : "ready";
  char lastFeed[24];
  snprintf(lastFeed, sizeof(lastFeed), "%u", (unsigned)g_cfg.lastFeedEpoch);
  snprintf(page, sizeof(page),
           "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta http-equiv=\"refresh\" content=\"10\">"
           "<title>Catfish Feeder Relay</title></head>"
           "<body><h2>Catfish Feeder &mdash; Relay Unit</h2>"
           "<p>State: <b>%s</b></p>"
           "<p>Last feed epoch: %s%s</p>"
           "<p>IP: %s</p>"
           "<p>Firmware: %s | Uptime: %u s | Free heap: %u B</p>"
           "<p>API: GET /health, GET /status, POST/GET /feed, POST /config</p>"
           "</body></html>",
           state, lastFeed, ntpTimeSynced() ? " (NTP)" : " (boot-seconds, NTP not synced)",
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

  bool changed = false;

  // Optional: rotate the token ("newToken"; "token" stays the current credential).
  if (doc["newToken"].is<const char*>()) {
    const char* nt = doc["newToken"].as<const char*>();
    if (nt != nullptr && nt[0] != '\0') {
      if (strlen(nt) >= RELAY_TOKEN_LEN) {
        sendJsonError(400, "token_too_long");
        return;
      }
      snprintf(g_cfg.token, sizeof(g_cfg.token), "%s", nt);
      changed = true;
    }
  }

  if (doc["maxPulseS"].is<long>()) {
    const long v = doc["maxPulseS"].as<long>();
    if (v < 1 || v > (long)MAX_PULSE_LIMIT_S) {
      sendJsonError(400, "maxPulseS_range_1_60");
      return;
    }
    g_cfg.maxPulseS = (uint8_t)v;
    changed = true;
  }

  if (doc["pulseS"].is<long>()) {
    long v = doc["pulseS"].as<long>();
    if (v < 1 || v > (long)PULSE_S_LIMIT) {
      sendJsonError(400, "pulseS_range_1_30");
      return;
    }
    if (v > (long)g_cfg.maxPulseS) v = (long)g_cfg.maxPulseS;  // keep pulseS <= maxPulseS
    g_cfg.pulseS = (uint8_t)v;
    changed = true;
  }

  if (changed) saveConfig();

  // Echo the effective config back to the authenticated caller (LAN convenience).
  JsonDocument echo;
  echo["ok"] = true;
  echo["changed"] = changed;
  echo["token"] = g_cfg.token;
  echo["maxPulseS"] = (int)g_cfg.maxPulseS;
  echo["pulseS"] = (int)g_cfg.pulseS;
  char buf[192];
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

  Serial.begin(115200);
  LOG("boot v%s (relay D1 active-%s, LED D4) -- relay forced INACTIVE at boot",
      FIRMWARE_VERSION, (RELAY_ACTIVE_LEVEL == LOW) ? "LOW" : "HIGH");

  configLoad();

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

  // mDNS on ESP8266 runs from lwIP timers (unlike ESP32, no MDNS.update() call).
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    LOG("mDNS: http://%s.local", HOSTNAME);
  } else {
    LOG("mDNS begin FAILED (continuing without .local name)");
  }

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.begin();  // open (no password) -- LAN-only appliance, documented
  LOG("OTA ready (LAN, no password)");

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
  pulseEngineTick();   // FIRST: relay must release on time even under network load
  statusLedTick();
  g_server.handleClient();
  ArduinoOTA.handle();
  discoveryPump();
  wifiWatchdog();
  yield();  // ESP8266 SDK / WiFi stack housekeeping
}
