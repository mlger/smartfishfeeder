/**
 * @file main.cpp
 * @brief Catfish Feeder display unit — ESP32-S3-Touch-LCD-4.3B (800x480 RGB).
 *
 * Architecture:
 *  - LVGL runs in its own FreeRTOS task (lvgl_v8_port), touch via GT911.
 *  - "wifi-task" (core 0) owns WiFiManager/mDNS/NTP.
 *  - "net-worker" (core 0) owns ALL blocking relay HTTP/discovery calls.
 *    loop() never blocks: it enqueues commands and drains results.
 *  - loop(): drains UI events + worker results every pass (~50 ms),
 *    1 Hz scheduler tick + persist flush + retry logic + ui_tick(),
 *    30 s WiFi UI refresh, 60 s relay probe (via worker).
 *
 * Scheduled-feed resilience: a failed scheduled feed is retried once ~30 s
 * later; persistent failure surfaces a "Feed FAILED" toast.
 *
 * Serial log tags: [FEED] [NET] [UI] [SYS].
 */
#include <Arduino.h>
#include <time.h>

#include <ArduinoOTA.h>

#include "lvgl_v8_port.h"
#include "esp_display_panel.hpp"

#include "feed_scheduler.h"
#include "net_wifi.h"
#include "net_worker.h"
#include "mqtt_mgr.h"
#include "prefs.h"
#include "relay_client.h"
#include "web_server.h"
#include "ui/ui.h"
#include <catfish_protocol.h>

using namespace esp_panel::board;
using namespace ui; // UI entry points (ui_init/ui_tick/ui_set_*/UiEvent)

// ------------------------------------------------------------------ OTA
// ArduinoOTA is open by default on this LAN-only appliance (same decision as
// the relay unit). To require a password, add -DOTA_PASSWORD=\"s3cret\" to
// build_flags (or inject it from CI). Empty string keeps the open behaviour.
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

static Board *s_board = nullptr;

// loop() cadence timestamps
static uint32_t s_t_1s = 0;
static uint32_t s_t_30s = 0;
static uint32_t s_t_60s = 0;

// Scheduled-feed retries: one delayed attempt per failed scheduled feed.
// Two slots cover the (rare) case of two entries failing in the same ~30 s
// window. Slots are armed on HTTP failure (30 s) or WiFi-down at fire time
// (60 s), and fire from the 1 Hz block in loop().
struct SchedRetry {
  bool pending = false;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t seconds = 0;
  uint32_t dueMs = 0;
};
static SchedRetry s_retries[2];

// Optimistic "Feeding..." on the Home card: set right after a successful feed
// (the relay IS running its pulse then), expired after the pulse + 2 s, or
// superseded early by the next real /status probe.
static bool s_opt_feeding = false;
static uint32_t s_opt_feeding_until = 0;

// Last relay state, mirrored to MQTT (retained) whenever it changes.
static bool s_relay_online_cached = false;
static bool s_feeding_cached = false;

// MQTT status as last painted on the Settings card (0xFF = force first sync).
static uint8_t s_mqtt_last_ui = 0xFF;

static void clearRetryFor(uint8_t hour, uint8_t minute) {
  for (SchedRetry &s : s_retries) {
    if (s.pending && s.hour == hour && s.minute == minute) s.pending = false;
  }
}

static bool armRetry(uint8_t hour, uint8_t minute, uint8_t seconds,
                     uint32_t delay_ms) {
  for (SchedRetry &s : s_retries) {
    if (!s.pending) {
      s.pending = true;
      s.hour = hour;
      s.minute = minute;
      s.seconds = seconds;
      s.dueMs = millis() + delay_ms;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static String fmtHhmm(uint8_t h, uint8_t m) {
  char b[8];
  snprintf(b, sizeof(b), "%02u:%02u", (unsigned)h, (unsigned)m);
  return String(b);
}

static String formatLastFed(uint32_t epoch) {
  if (epoch == 0) return String("Never");
  const time_t t = (time_t)epoch;
  struct tm lt;
  if (!localtime_r(&t, &lt)) return String("Never");

  const time_t now = time(nullptr);
  struct tm ln;
  localtime_r(&now, &ln);

  char buf[40];
  if (ln.tm_year == lt.tm_year && ln.tm_yday == lt.tm_yday)
    snprintf(buf, sizeof(buf), "Today %02d:%02d", lt.tm_hour, lt.tm_min);
  else
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", lt.tm_year + 1900,
             lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
  return String(buf);
}

/** Backlight on/off handler registered with the UI (switch, no PWM). */
static void appSetBacklight(bool on) {
  if (s_board && s_board->getBacklight()) {
    if (on)
      s_board->getBacklight()->on();
    else
      s_board->getBacklight()->off();
  }
  Serial.printf("[SYS] backlight %s\n", on ? "ON" : "OFF");
}

/** Recompute next feeding from the scheduler and push it to the UI + MQTT. */
static void refreshNextFeedUi() {
  if (!feed_scheduler::timeValid()) {
    lvgl_port_lock(-1);
    ui_set_next_feed("Waiting for time sync...");
    lvgl_port_unlock();
    return;
  }
  const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
  lvgl_port_lock(-1);
  if (nf.valid)
    ui_set_next_feed_time(nf.hour, nf.minute, nf.tomorrow, true);
  else
    ui_set_next_feed("No feedings scheduled");
  lvgl_port_unlock();
  // "HH:MM" plain string (retained); 'none' when the schedule is empty. Not
  // published before NTP is valid — the last value stays until we know.
  if (nf.valid)
    mqtt_mgr::publishNextFeed(fmtHhmm(nf.hour, nf.minute));
  else
    mqtt_mgr::publishNextFeed(String("none"));
}

static void uiToast(const String &msg) {
  lvgl_port_lock(-1);
  ui_toast(msg);
  lvgl_port_unlock();
}

// ---------------------------------------------------------------------------
// MQTT <-> UI sync (loop context only)
// ---------------------------------------------------------------------------
static String mqttBrokerSummary() {
  if (!prefs::getMqttEnabled()) return String("Broker: not set");
  return String("Broker: ") + prefs::getMqttHost() + ":" +
         (unsigned)prefs::getMqttPort() + "  (" + prefs::getMqttPrefix() + "/...)";
}

static void mqttSyncUi() {
  const mqtt_mgr::Status st = mqtt_mgr::status();
  if ((uint8_t)st == s_mqtt_last_ui) return;
  s_mqtt_last_ui = (uint8_t)st;
  const char *txt = "disabled";
  uint8_t state = 0;
  switch (st) {
    case mqtt_mgr::Status::Disabled:   txt = "disabled";           state = 0; break;
    case mqtt_mgr::Status::Offline:    txt = "offline - retrying"; state = 1; break;
    case mqtt_mgr::Status::Connecting: txt = "connecting...";      state = 1; break;
    case mqtt_mgr::Status::Connected:  txt = "connected";          state = 2; break;
  }
  lvgl_port_lock(-1);
  ui_set_mqtt_status(String(txt), state);
  ui_set_mqtt_broker(mqttBrokerSummary());
  lvgl_port_unlock();
}

/** "HH:MM" of the next enabled feeding ("none"/"" per scheduler state). */
static String nextFeedHhmm() {
  if (!feed_scheduler::timeValid()) return String("");
  const feed_scheduler::NextFeed n = feed_scheduler::nextOccurrence();
  return n.valid ? fmtHhmm(n.hour, n.minute) : String("none");
}

/** Full retained state snapshot (called after every (re)connect). */
static void mqttPublishAllState() {
  mqtt_mgr::publishLastFeed(prefs::getLastFedEpoch());
  mqtt_mgr::publishNextFeed(nextFeedHhmm());
  mqtt_mgr::publishRelayOnline(s_relay_online_cached);
  mqtt_mgr::publishFeeding(s_feeding_cached);
  mqtt_mgr::publishPulse(prefs::getDefaultPulse());
}

/** Refresh the plain-field cache the web-task serves /api/status from. */
static void webPublishSnapshot() {
  web_server::Snapshot s;
  s.wifiUp = net_wifi::wifi_is_connected();
  strlcpy(s.wifiSsid, net_wifi::wifi_ssid().c_str(), sizeof(s.wifiSsid));
  strlcpy(s.wifiIp, net_wifi::wifi_ip().c_str(), sizeof(s.wifiIp));
  s.rssi = (int16_t)net_wifi::wifi_rssi();
  s.relayOnline = s_relay_online_cached;
  s.feeding = s_feeding_cached;
  strlcpy(s.relayIp, relay_client::ip().c_str(), sizeof(s.relayIp));
  const String nf = nextFeedHhmm();
  strlcpy(s.nextFeed, nf.c_str(), sizeof(s.nextFeed));
  s.lastFedEpoch = prefs::getLastFedEpoch();
  s.pulse = prefs::getDefaultPulse();
  s.schedCount = (uint16_t)feed_scheduler::getCount();
  s.clockValid = feed_scheduler::timeValid();
  strlcpy(s.version, CATFISH_APP_VERSION, sizeof(s.version));
  const mqtt_mgr::Status st = mqtt_mgr::status();
  const char *mtxt = "disabled";
  s.mqttState = 0;
  switch (st) {
    case mqtt_mgr::Status::Offline:    mtxt = "offline";    s.mqttState = 1; break;
    case mqtt_mgr::Status::Connecting: mtxt = "connecting"; s.mqttState = 1; break;
    case mqtt_mgr::Status::Connected:  mtxt = "connected";  s.mqttState = 2; break;
    case mqtt_mgr::Status::Disabled:   break;
  }
  strlcpy(s.mqtt, mtxt, sizeof(s.mqtt));
  web_server::publishSnapshot(s);
}

// ---------------------------------------------------------------------------
// ArduinoOTA (display unit): begun lazily on the first loop() pass AFTER WiFi
// connects, because mDNS ('catfishfeeder') is registered inside wifi-task and
// the Arduino core advertises the _arduino/_tcp service through that shared
// mDNS instance. handle() is pumped from loop() (~50 ms cadence); callbacks
// fire in the same task context as every other UI update, so toasts are safe.
// ---------------------------------------------------------------------------
static bool s_ota_ready = false;

static void beginOtaOnce() {
  if (s_ota_ready || !net_wifi::wifi_is_connected()) return;

  ArduinoOTA.setHostname(CATFISH_DISPLAY_HOSTNAME);

  ArduinoOTA.onStart([] {
    Serial.println("[SYS] OTA update started (firmware)");
    uiToast("Receiving update...");
  });
  ArduinoOTA.onEnd([] {
    Serial.println("[SYS] OTA update finished, rebooting");
    uiToast("Update OK - restarting");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static uint8_t s_last_step = 0; // log at 25% steps, not per chunk
    if (!total) return;
    const uint8_t step = (uint8_t)((done * 100U / total) / 25U);
    if (step != s_last_step) {
      s_last_step = step;
      Serial.printf("[SYS] OTA progress %u%%\n", (unsigned)(done * 100U / total));
    }
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("[SYS] OTA error %d\n", (int)err);
    uiToast("Update failed (code " + String((int)err) + ")");
  });

  if (OTA_PASSWORD[0] != '\0') {
    ArduinoOTA.setPassword(OTA_PASSWORD);
    Serial.println("[SYS] OTA ready: catfishfeeder.local:3232 (password protected)");
  } else {
    Serial.println("[SYS] OTA ready: catfishfeeder.local:3232 (LAN, no password)");
  }
  ArduinoOTA.begin();
  s_ota_ready = true;
}

/** Route a feed request to the worker; manual feeds get immediate feedback. */
static void requestFeed(uint8_t seconds, bool manual, uint8_t hour,
                        uint8_t minute, bool retry) {
  if (!net_wifi::wifi_is_connected()) {
    if (manual) {
      uiToast("WiFi not connected");
    } else if (!retry) {
      // H2: a scheduled feed must survive a WiFi blip at fire time - arm a
      // delayed attempt instead of silently dropping it.
      if (armRetry(hour, minute, seconds, 60000UL)) {
        uiToast("No WiFi - feed will retry");
      } else {
        uiToast(String("Feed FAILED: no WiFi"));
      }
    } else {
      uiToast(String("Feed FAILED: no WiFi"));
    }
    Serial.println("[FEED] aborted: WiFi down");
    return;
  }
  if (!net_worker::enqueueFeed(seconds, manual, hour, minute, retry)) {
    uiToast("Too busy, try again");
    Serial.println("[FEED] worker queue full, request dropped");
    return;
  }
  if (manual) {
    lvgl_port_lock(-1);
    ui_toast("Feeding fish...");
    ui_set_feed_busy(true);
    lvgl_port_unlock();
  }
}

/** Apply a schedule change: persist + rebuild list + refresh next-feed. */
static void afterScheduleChange(bool removed) {
  feed_scheduler::flushIfDirty();
  lvgl_port_lock(-1);
  ui_schedule_refresh();
  ui_toast(removed ? "Feeding time removed" : "Feeding time added");
  ui_editor_close(); // no-op unless the editor overlay is open
  lvgl_port_unlock();
  refreshNextFeedUi();
}

// ---------------------------------------------------------------------------
// UI event handling (loop context; never blocks except in-event flash writes)
// ---------------------------------------------------------------------------
static void handleUiEvent(const UiEvent &ev) {
  switch (ev.type) {
    case UI_EVT_FEED:
      Serial.printf("[FEED] manual feed %u s\n", (unsigned)ev.a);
      requestFeed(ev.a, true, 0, 0, false);
      break;

    case UI_EVT_RELAY_TEST:
      Serial.println("[NET] relay test requested");
      if (net_worker::enqueueProbe(true)) {
        lvgl_port_lock(-1);
        ui_set_probe_busy(true);
        lvgl_port_unlock();
      } else {
        uiToast("Too busy, try again");
      }
      break;

    case UI_EVT_REBOOT:
      Serial.println("[SYS] reboot requested");
      feed_scheduler::flushIfDirty();
      delay(300);
      ESP.restart();
      break;

    case UI_EVT_SET_PULSE:
      prefs::setDefaultPulse(ev.a);
      ui_set_pulse(ev.a); // also corrects the spinbox when set via Home Assistant
      mqtt_mgr::publishPulse(ev.a); // retained; no-op when disconnected
      Serial.printf("[FEED] default pulse set to %u s\n", (unsigned)ev.a);
      break;

    case UI_EVT_SET_TOKEN:
      if (ev.flag) {
        // M4: rotate on the relay unit first (POST /config newToken); the new
        // token is only persisted locally once the relay accepted it.
        if (net_worker::enqueueTokenRotate(ev.token)) {
          uiToast("Updating relay token...");
        } else {
          uiToast("Too busy - relay not updated");
        }
      } else {
        prefs::setRelayToken(String(ev.token));
        uiToast("Pairing token updated");
      }
      Serial.println("[NET] pairing token update requested");
      break;

    case UI_EVT_SET_BL:
      prefs::setBacklightOn(ev.flag);
      Serial.printf("[SYS] backlight preference saved: %s\n",
                    ev.flag ? "on" : "off");
      break;

    case UI_EVT_SCHED_ADD: {
      const bool full =
          feed_scheduler::getCount() >= feed_scheduler::MAX_FEED_TIMES;
      const FeedTime t{ev.a, ev.b, true, ev.c};
      if (feed_scheduler::add(t)) {
        afterScheduleChange(false);
      } else {
        uiToast(full ? "Schedule full (max 8)" : "Time already scheduled");
        Serial.println("[FEED] schedule add rejected");
      }
      break;
    }

    case UI_EVT_SCHED_REMOVE:
      if (feed_scheduler::removeByIdentity(ev.a, ev.b, ev.c)) {
        afterScheduleChange(true);
      } else {
        Serial.println("[FEED] schedule remove: entry not found");
      }
      break;

    case UI_EVT_SCHED_PERSIST:
      feed_scheduler::setEnabled(ev.a, ev.flag); // single-writer flip (loop ctx)
      feed_scheduler::flushIfDirty();
      refreshNextFeedUi(); // enable/disable changes the next occurrence
      break;

    case UI_EVT_MQTT_CFG:
      prefs::setMqttEnabled(ev.mqttEnable);
      prefs::setMqttHost(String(ev.mqttHost));
      prefs::setMqttPort(ev.mqttPort);
      prefs::setMqttUser(String(ev.mqttUser));
      prefs::setMqttPass(String(ev.mqttPass));
      prefs::setMqttPrefix(String(ev.mqttPrefix));
      Serial.printf("[SYS] MQTT settings saved (en=%u %s:%u prefix '%s')\n",
                    ev.mqttEnable ? 1 : 0, ev.mqttHost, (unsigned)ev.mqttPort,
                    ev.mqttPrefix);
      mqtt_mgr::applyConfig();
      mqttSyncUi();
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Worker result handling
// ---------------------------------------------------------------------------
static void handleWorkerResult(const net_worker::Result &r) {
  if (r.kind == net_worker::Result::RES_TOKEN) {
    if (r.ok) {
      prefs::setRelayToken(String(r.token));
      uiToast("Relay token updated");
      Serial.printf("[NET] relay token rotated OK (ip %s)\n", r.ip);
    } else {
      uiToast(String("Token rotate failed: ") + r.error);
      Serial.printf("[NET] relay token rotate FAILED: %s\n", r.error);
    }
    return;
  }

  if (r.kind == net_worker::Result::RES_FEED) {
    if (r.ok) {
      if (!r.manual) clearRetryFor(r.hour, r.minute);

      // M8: only trust the wall clock (and persist lastFed) once NTP synced.
      const bool clock_ok = feed_scheduler::timeValid();
      String when;
      if (clock_ok) {
        const uint32_t epoch = (uint32_t)time(nullptr);
        prefs::setLastFedEpoch(epoch);
        when = r.manual ? formatLastFed(epoch)
                        : (String("Today ") + fmtHhmm(r.hour, r.minute));
      }

      lvgl_port_lock(-1);
      ui_set_feed_busy(false);
      if (clock_ok) ui_set_last_feed(when);
      ui_set_relay(true, String(r.ip), true); // M2: optimistic feeding state
      if (r.manual) {
        String t = String("Fed OK (") + r.seconds + " s)";
        if (r.clamped) t += " - clamped"; // M3: relay clamp surfaced
        ui_toast(t);
      } else {
        ui_toast(String("Fed ") + fmtHhmm(r.hour, r.minute));
      }
      lvgl_port_unlock();

      if (r.clamped)
        Serial.printf("[FEED] relay clamp note: %s\n", r.note);
      s_opt_feeding = true;
      s_opt_feeding_until = millis() + (uint32_t)r.seconds * 1000UL + 2000UL;
      s_relay_online_cached = true;
      s_feeding_cached = true;
      // Retained MQTT mirror: the relay IS pulsing now (M2-consistent).
      mqtt_mgr::publishFeeding(true);
      if (clock_ok) mqtt_mgr::publishLastFeed(prefs::getLastFedEpoch());
      Serial.printf("[FEED] feed OK via %s (%u s)\n", r.ip, (unsigned)r.seconds);
      refreshNextFeedUi();
    } else if (r.manual) {
      lvgl_port_lock(-1);
      ui_set_feed_busy(false);
      ui_toast(String("Feed failed: ") + r.error);
      lvgl_port_unlock();
      Serial.printf("[FEED] manual feed FAILED: %s\n", r.error);
    } else {
      // scheduled feed
      if (r.retry) {
        uiToast(String("Feed FAILED: ") + r.error);
        Serial.printf("[FEED] scheduled feed FAILED (no more retries): %s\n",
                      r.error);
      } else if (armRetry(r.hour, r.minute, r.seconds, 30000UL)) {
        uiToast("Feed failed - retrying in 30 s");
        Serial.printf("[FEED] scheduled feed failed (%s), retry armed\n",
                      r.error);
      } else {
        uiToast(String("Feed FAILED: ") + r.error);
        Serial.println("[FEED] scheduled feed FAILED (retry slots full)");
      }
    }
    return;
  }

  // RES_PROBE
  s_opt_feeding = false; // a real /status result supersedes the optimistic state
  s_relay_online_cached = r.online;
  s_feeding_cached = r.online && r.feeding;
  lvgl_port_lock(-1);
  ui_set_probe_busy(false);
  ui_set_relay(r.online, r.ip, r.feeding);
  lvgl_port_unlock();
  // Retained MQTT mirror of the relay state (no-ops when disconnected).
  mqtt_mgr::publishRelayOnline(r.online);
  if (r.online) mqtt_mgr::publishFeeding(r.feeding);
  if (r.report) {
    if (!net_wifi::wifi_is_connected())
      ui_toast("WiFi not connected");
    else if (r.online) {
      String t = String("Relay OK  v") + r.version;
      if (r.protoMismatch) t += " (protocol mismatch!)";
      ui_toast(t);
    } else
      ui_toast("Relay unreachable");
  }
  lvgl_port_unlock();
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[SYS] Catfish Feeder display unit booting (v" CATFISH_APP_VERSION ")");

  // --- persistence ---
  prefs::begin();
  feed_scheduler::init();
  relay_client::init();
  if (!mqtt_mgr::begin()) {
    Serial.println("[SYS] WARN: MQTT manager unavailable (continuing without)");
  }
  if (!web_server::begin()) {
    Serial.println("[SYS] WARN: web dashboard unavailable (continuing without)");
  }

  // --- board + LVGL ---
  s_board = new Board();
  s_board->init();
  if (!s_board->begin()) {
    Serial.println("[SYS] ERROR: board begin() failed - halting");
    while (true) delay(1000);
  }
  if (s_board->getBacklight()) {
    s_board->getBacklight()->setBrightness(1); // switch type: on (no PWM)
  }

  if (!lvgl_port_init(s_board->getLCD(), s_board->getTouch())) {
    Serial.println("[SYS] ERROR: LVGL port init failed");
  }

  // build UI under lock
  lvgl_port_lock(-1);
  ui_init();
  ui_set_backlight_cb(appSetBacklight, prefs::getBacklightOn());
  ui_set_relay(false, prefs::getRelayIp());
  ui_set_last_feed(formatLastFed(prefs::getLastFedEpoch()));
  lvgl_port_unlock();
  refreshNextFeedUi();

  // --- background tasks ---
  if (!net_worker::begin()) {
    Serial.println("[SYS] ERROR: net worker unavailable; feeds will fail");
  }
  net_wifi::start();
  mqtt_mgr::applyConfig(); // arms retry pump (WiFi-gated); status -> UI via 1 Hz sync
  mqttSyncUi();

  // prime loop() cadence: first relay probe ~15 s after boot
  s_t_1s = millis();
  s_t_30s = millis();
  s_t_60s = millis() - 45000UL;

  Serial.println("[SYS] setup complete");
}

void loop() {
  const uint32_t now = millis();

  // OTA updater: begin lazily once WiFi/mDNS is up, then serve every pass.
  beginOtaOnce();
  if (s_ota_ready) ArduinoOTA.handle();

  // ---- drain UI events + worker results (every pass, ~50 ms latency) ----
  UiEvent ev;
  while (ui_event_receive(ev)) handleUiEvent(ev);

  net_worker::Result res;
  while (net_worker::receive(res)) handleWorkerResult(res);

  // ---- web dashboard: apply schedule replacements (authoritative path) ----
  char *json = nullptr;
  while (web_server::takeScheduleReplace(&json)) {
    if (feed_scheduler::replaceAll(String(json))) {
      feed_scheduler::flushIfDirty();
      lvgl_port_lock(-1);
      ui_schedule_refresh();
      lvgl_port_unlock();
      uiToast("Schedule updated from dashboard");
      refreshNextFeedUi();
    } else {
      Serial.println("[WEB] schedule replace rejected by scheduler");
      uiToast("Dashboard schedule rejected");
    }
    free(json);
  }

  // ---- drain MQTT connection events (connected -> dump discovery+state) ----
  bool mqConn = false;
  while (mqtt_mgr::receive(mqConn)) {
    if (mqConn) {
      mqtt_mgr::publishDiscovery();
      mqtt_mgr::publishAvailable();
      mqttPublishAllState();
    }
    mqttSyncUi();
  }

  // ---------------- 1 Hz block ----------------
  if ((now - s_t_1s) >= 1000) {
    s_t_1s = now;

    // catch-all persist flush (covers setEnabled-only edits)
    feed_scheduler::flushIfDirty();

    // scheduled feeding
    const FeedTime due = feed_scheduler::tick();
    if (due.enabled) {
      Serial.printf("[FEED] scheduled feed %02u:%02u (%u s)\n",
                    (unsigned)due.hour, (unsigned)due.minute,
                    (unsigned)due.seconds);
      uiToast("Feeding time!");
      requestFeed(due.seconds, false, due.hour, due.minute, false);
    }

    // armed retries for failed scheduled feeds (one delayed attempt each)
    for (SchedRetry &s : s_retries) {
      if (s.pending && (int32_t)(now - s.dueMs) >= 0) {
        s.pending = false;
        Serial.printf("[FEED] retrying scheduled feed %02u:%02u (%u s)\n",
                      (unsigned)s.hour, (unsigned)s.minute,
                      (unsigned)s.seconds);
        requestFeed(s.seconds, false, s.hour, s.minute, true);
      }
    }

    // end of the optimistic feeding indicator (pulse + 2 s elapsed)
    if (s_opt_feeding && (int32_t)(now - s_opt_feeding_until) >= 0) {
      s_opt_feeding = false;
      s_feeding_cached = false;
      lvgl_port_lock(-1);
      ui_set_relay(true, relay_client::ip(), false);
      lvgl_port_unlock();
      mqtt_mgr::publishFeeding(false);
    }

    // MQTT reconnect pump + Settings-card status sync
    mqtt_mgr::retryIfDue(now);
    mqttSyncUi();

    // web dashboard state cache (~1 Hz; plain-field reads for the web task)
    webPublishSnapshot();

    // UI housekeeping (clock, countdown, relay dot, idle dim)
    lvgl_port_lock(-1);
    ui_tick();
    lvgl_port_unlock();
  }

  // ---------------- 30 s block: WiFi status to UI ----------------
  if ((now - s_t_30s) >= 30000) {
    s_t_30s = now;
    lvgl_port_lock(-1);
    ui_set_wifi(net_wifi::wifi_is_connected(), net_wifi::wifi_ssid(),
                net_wifi::wifi_ip(), net_wifi::wifi_rssi());
    lvgl_port_unlock();
  }

  // ---------------- 60 s block: relay probe (via worker, only if WiFi up) ----
  if ((now - s_t_60s) >= 60000) {
    s_t_60s = now;
    if (net_wifi::wifi_is_connected()) {
      net_worker::enqueueProbe(false);
    }
  }

  delay(50);
}
