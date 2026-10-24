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
#include <catfish_mech.h>

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
  uint8_t tankMask = 0; // P5: retry the same target tanks
  uint32_t dueMs = 0;
};
static SchedRetry s_retries[2];

// Optimistic "Feeding..." per tank: set right after a successful feed (the
// relay IS running its pulse then), expired after the pulse + 2 s, or
// superseded early by the next real /status probe. Index = tank-1.
static bool s_opt_feeding[relay_client::MAX_TANKS] = {false, false, false, false};
static uint32_t s_opt_feeding_until[relay_client::MAX_TANKS] = {0, 0, 0, 0};

// Last relay state per tank (probe results + optimistic state merged).
static bool s_tank_online[relay_client::MAX_TANKS] = {false, false, false, false};
static bool s_tank_feeding[relay_client::MAX_TANKS] = {false, false, false, false};

// Selected tank on the Home screen: 0-based index or 0xFE = "All".
static uint8_t s_selected_tank = 0;

// Aggregate state mirrored to MQTT (retained) whenever it changes:
// relay_online = ANY configured tank online; feeding = ANY tank feeding.
static bool s_agg_online_cached = false;
static bool s_agg_feeding_cached = false;

// MQTT status as last painted on the Settings card (0xFF = force first sync).
static uint8_t s_mqtt_last_ui = 0xFF;

static void clearRetryFor(uint8_t hour, uint8_t minute) {
  for (SchedRetry &s : s_retries) {
    if (s.pending && s.hour == hour && s.minute == minute) s.pending = false;
  }
}

static bool armRetry(uint8_t hour, uint8_t minute, uint8_t seconds,
                     uint32_t delay_ms, uint8_t tankMask) {
  for (SchedRetry &s : s_retries) {
    if (!s.pending) {
      s.pending = true;
      s.hour = hour;
      s.minute = minute;
      s.seconds = seconds;
      s.tankMask = tankMask;
      s.dueMs = millis() + delay_ms;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Multi-tank helpers (loop context)
// ---------------------------------------------------------------------------
static uint8_t tankCountClamped() {
  uint8_t c = prefs::getTankCount();
  if (c < 1) c = 1;
  if (c > relay_client::MAX_TANKS) c = relay_client::MAX_TANKS;
  return c;
}

static uint8_t configuredTankMask() {
  const uint8_t c = tankCountClamped();
  return (c >= 4) ? 0x0F : (uint8_t)((1u << c) - 1u);
}

/** Publish retained aggregate MQTT state only when it actually changed. */
static void recomputeAggregates() {
  bool anyOnline = false, anyFeeding = false;
  const uint8_t count = tankCountClamped();
  for (uint8_t i = 0; i < count; i++) {
    anyOnline |= s_tank_online[i];
    anyFeeding |= s_tank_feeding[i];
  }
  if (anyOnline != s_agg_online_cached) {
    s_agg_online_cached = anyOnline;
    mqtt_mgr::publishRelayOnline(anyOnline);
  }
  if (anyFeeding != s_agg_feeding_cached) {
    s_agg_feeding_cached = anyFeeding;
    mqtt_mgr::publishFeeding(anyFeeding);
  }
}

/** Repaint the Home relay card for the selected tank (or the aggregate). */
static void refreshSelectedTankCard() {
  const uint8_t count = tankCountClamped();
  if (s_selected_tank == 0xFE) {
    uint8_t on = 0;
    bool anyFeed = false;
    for (uint8_t i = 0; i < count; i++) {
      on += s_tank_online[i] ? 1 : 0;
      anyFeed |= s_tank_feeding[i];
    }
    lvgl_port_lock(-1);
    ui_set_relay(on == count, String(""), anyFeed);
    lvgl_port_unlock();
  } else {
    const uint8_t i = (s_selected_tank < count) ? s_selected_tank : 0;
    lvgl_port_lock(-1);
    ui_set_relay(s_tank_online[i], relay_client::ip(i + 1), s_tank_feeding[i]);
    lvgl_port_unlock();
  }
}

/** Push prefs tank names into the UI (chips + rows + editor checkboxes). */
static void syncTanksUi() {
  String names[relay_client::MAX_TANKS];
  for (uint8_t i = 0; i < relay_client::MAX_TANKS; i++)
    names[i] = prefs::getTankName(i + 1);
  lvgl_port_lock(-1);
  ui_tanks_rebuild(tankCountClamped(), names, s_selected_tank);
  lvgl_port_unlock();
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
  mqtt_mgr::publishRelayOnline(s_agg_online_cached);
  mqtt_mgr::publishFeeding(s_agg_feeding_cached);
  mqtt_mgr::publishPulse(prefs::getDefaultPulse());
  const uint8_t count = tankCountClamped();
  for (uint8_t t = 1; t <= count; t++) {
    mqtt_mgr::publishTankOnline(t, s_tank_online[t - 1]);
  }
}

/** Refresh the plain-field cache the web-task serves /api/status from. */
static void webPublishSnapshot() {
  web_server::Snapshot s;
  s.wifiUp = net_wifi::wifi_is_connected();
  strlcpy(s.wifiSsid, net_wifi::wifi_ssid().c_str(), sizeof(s.wifiSsid));
  strlcpy(s.wifiIp, net_wifi::wifi_ip().c_str(), sizeof(s.wifiIp));
  s.rssi = (int16_t)net_wifi::wifi_rssi();
  const uint8_t count = tankCountClamped();
  s.feeding = s_agg_feeding_cached;
  if (s_selected_tank == 0xFE) {
    bool allOnline = true;
    for (uint8_t i = 0; i < count; i++) allOnline &= s_tank_online[i];
    s.relayOnline = allOnline;
    strlcpy(s.relayIp, "", sizeof(s.relayIp));
  } else {
    const uint8_t i = (s_selected_tank < count) ? s_selected_tank : 0;
    s.relayOnline = s_tank_online[i];
    strlcpy(s.relayIp, relay_client::ip(i + 1).c_str(), sizeof(s.relayIp));
  }
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
  // multi-tank block
  s.tankCount = count;
  s.selectedTank = s_selected_tank;
  for (uint8_t i = 0; i < relay_client::MAX_TANKS; i++) {
    strlcpy(s.tankName[i], prefs::getTankName(i + 1).c_str(),
            sizeof(s.tankName[i]));
    s.tankOnline[i] = s_tank_online[i];
    s.tankFeeding[i] = s_tank_feeding[i];
    strlcpy(s.tankIp[i], relay_client::ip(i + 1).c_str(), sizeof(s.tankIp[i]));
  }
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
                        uint8_t minute, bool retry, uint8_t tankMask) {
  if (!net_wifi::wifi_is_connected()) {
    if (manual) {
      uiToast("WiFi not connected");
    } else if (!retry) {
      // H2: a scheduled feed must survive a WiFi blip at fire time - arm a
      // delayed attempt instead of silently dropping it.
      if (armRetry(hour, minute, seconds, 60000UL, tankMask)) {
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
  if (!net_worker::enqueueFeed(seconds, manual, hour, minute, retry, tankMask)) {
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
    case UI_EVT_FEED: {
      Serial.printf("[FEED] manual feed %u s (mask 0x%X)\n", (unsigned)ev.a,
                    (unsigned)ev.tankMask);
      // explicit mask (MQTT tank button / dashboard) or the Home selection
      const uint8_t mask = ev.tankMask ? ev.tankMask
                                       : (s_selected_tank == 0xFE
                                              ? configuredTankMask()
                                              : (uint8_t)(1u << s_selected_tank));
      requestFeed(ev.a, true, 0, 0, false, mask);
      break;
    }

    case UI_EVT_RELAY_TEST: {
      Serial.println("[NET] relay test requested");
      // Test the SELECTED tank (tank 1 when "All" is selected).
      const uint8_t tank1 =
          (s_selected_tank == 0xFE) ? 1 : (uint8_t)(s_selected_tank + 1);
      if (net_worker::enqueueProbe(true, tank1)) {
        lvgl_port_lock(-1);
        ui_set_probe_busy(true);
        lvgl_port_unlock();
      } else {
        uiToast("Too busy, try again");
      }
      break;
    }

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
        // M4: rotate the TANK 1 relay token first (POST /config newToken); the
        // new token is only persisted locally once the relay accepted it.
        // Per-tank token overrides are edited per tank (Settings > TANKS).
        if (net_worker::enqueueTokenRotate(ev.token, 1)) {
          uiToast("Updating relay token...");
        } else {
          uiToast("Too busy - relay not updated");
        }
      } else {
        prefs::setRelayToken(String(ev.token));
        relay_client::applyTankTokens(); // tanks without override inherit it
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
      const FeedTime t{ev.a, ev.b, true, ev.c, ev.tankMask};
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

    case UI_EVT_MECH_CFG: {
      // P4: POST /config on the selected tank's relay; RES_MECH reports back.
      const uint8_t tank1 =
          (s_selected_tank == 0xFE) ? 1 : (uint8_t)(s_selected_tank + 1);
      if (net_worker::enqueueMech(ev.mechId, ev.mechRest, ev.mechFeed,
                                  ev.mechHz, ev.mechDir, tank1)) {
        Serial.printf("[SYS] mechanism change queued: %s (tank %u)\n",
                      catfishMechName(ev.mechId), (unsigned)tank1);
      } else {
        uiToast("Too busy - mechanism not changed");
      }
      break;
    }

    case UI_EVT_SELECT_TANK:
      s_selected_tank = (ev.a == 0xFE) ? 0xFE : ev.a;
      syncTanksUi(); // re-highlight the chips
      refreshSelectedTankCard();
      Serial.printf("[SYS] selected tank: %s\n",
                    s_selected_tank == 0xFE ? "All"
                                            : prefs::getTankName(s_selected_tank + 1).c_str());
      break;

    case UI_EVT_TANK_COUNT: {
      const uint8_t prev = tankCountClamped();
      prefs::setTankCount(ev.a);
      const uint8_t nw = tankCountClamped();
      if (nw < prev) {
        // shrink: drop live state + HA entities for the removed tanks
        for (uint8_t i = nw; i < relay_client::MAX_TANKS; i++) {
          s_tank_online[i] = false;
          s_tank_feeding[i] = false;
          s_opt_feeding[i] = false;
        }
        mqtt_mgr::clearTankDiscovery(prev, nw);
      }
      if (s_selected_tank != 0xFE && s_selected_tank >= nw) s_selected_tank = 0;
      syncTanksUi();
      refreshSelectedTankCard();
      recomputeAggregates();
      uiToast(String("Tanks set to ") + nw);
      Serial.printf("[SYS] tank count %u -> %u\n", (unsigned)prev, (unsigned)nw);
      break;
    }

    case UI_EVT_TANK_CFG:
      prefs::setTankName(ev.a, String(ev.tankName));
      prefs::setTankToken(ev.a, String(ev.token));
      relay_client::applyTankTokens(); // token override may have changed
      syncTanksUi();
      uiToast("Tank updated");
      Serial.printf("[SYS] tank %u config saved (name '%s')\n", (unsigned)ev.a,
                    ev.tankName);
      break;

    case UI_EVT_SCAN_TANKS:
      if (net_worker::enqueueScanTanks()) {
        lvgl_port_lock(-1);
        ui_set_scan_busy(true);
        lvgl_port_unlock();
        uiToast("Scanning for relays...");
      } else {
        uiToast("Too busy, try again");
      }
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

  if (r.kind == net_worker::Result::RES_MECH) {
    if (r.ok) {
      uiToast("Mechanism updated");
      Serial.printf("[NET] mechanism updated OK (ip %s) - re-probing\n", r.ip);
      // refresh the Settings mechanism line for the tank that was changed
      net_worker::enqueueProbe(true, r.tankId ? r.tankId : 1);
    } else {
      uiToast(String("Mechanism update failed: ") + r.error);
      Serial.printf("[NET] mechanism update FAILED: %s\n", r.error);
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

      // Optimistic feeding state per successfully fed tank.
      for (uint8_t t = 1; t <= relay_client::MAX_TANKS; t++) {
        if (r.tanksOk & (1u << (t - 1))) {
          s_tank_feeding[t - 1] = true;
          s_opt_feeding[t - 1] = true;
          s_opt_feeding_until[t - 1] =
              millis() + (uint32_t)r.seconds * 1000UL + 2000UL;
        }
      }

      lvgl_port_lock(-1);
      ui_set_feed_busy(false);
      if (clock_ok) ui_set_last_feed(when);
      if (r.manual) {
        String t = String("Fed OK (") + r.seconds + " s";
        if (r.tanksOk & (r.tanksOk - 1)) t += ", multiple tanks";
        if (r.clamped) t += " - clamped"; // M3: relay clamp surfaced
        t += ")";
        ui_toast(t);
      } else {
        String t = String("Fed ") + fmtHhmm(r.hour, r.minute);
        if (r.tanksFail) t += " (some tanks failed)";
        ui_toast(t);
      }
      lvgl_port_unlock();
      refreshSelectedTankCard();

      if (r.clamped)
        Serial.printf("[FEED] relay clamp note: %s\n", r.note);
      if (r.tanksFail)
        Serial.printf("[FEED] partial feed: ok=0x%X fail=0x%X (%s)\n",
                      (unsigned)r.tanksOk, (unsigned)r.tanksFail, r.error);
      // Retained MQTT mirror: the relay IS pulsing now (M2-consistent).
      recomputeAggregates();
      if (clock_ok) mqtt_mgr::publishLastFeed(prefs::getLastFedEpoch());
      Serial.printf("[FEED] feed OK via %s (%u s, tanks ok=0x%X)\n", r.ip,
                    (unsigned)r.seconds, (unsigned)r.tanksOk);
      refreshNextFeedUi();
    } else if (r.manual) {
      lvgl_port_lock(-1);
      ui_set_feed_busy(false);
      ui_toast(String("Feed failed: ") + r.error);
      lvgl_port_unlock();
      Serial.printf("[FEED] manual feed FAILED: %s\n", r.error);
    } else {
      // scheduled feed (all target tanks failed): retry the failed set
      if (r.retry) {
        uiToast(String("Feed FAILED: ") + r.error);
        Serial.printf("[FEED] scheduled feed FAILED (no more retries): %s\n",
                      r.error);
      } else if (armRetry(r.hour, r.minute, r.seconds, 30000UL,
                          r.tanksFail ? r.tanksFail : 0x01)) {
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

  if (r.kind == net_worker::Result::RES_SCAN) {
    lvgl_port_lock(-1);
    ui_set_scan_busy(false);
    lvgl_port_unlock();
    if (r.ok) {
      String ips[relay_client::MAX_TANKS];
      for (uint8_t i = 0; i < relay_client::MAX_TANKS; i++)
        ips[i] = relay_client::ip(i + 1);
      lvgl_port_lock(-1);
      ui_tanks_state(s_tank_online, s_tank_feeding, ips);
      lvgl_port_unlock();
      String t = String("Scan: ") + r.liveCount + " relay(s) live";
      if (r.boundMask) t += ", " + String((int)__builtin_popcount(r.boundMask)) + " newly bound";
      uiToast(t);
      Serial.printf("[NET] tank scan done: live=%u bound=0x%X\n",
                    (unsigned)r.liveCount, (unsigned)r.boundMask);
      refreshSelectedTankCard();
    } else {
      uiToast(String("Scan failed: ") + r.error);
      Serial.printf("[NET] tank scan FAILED: %s\n", r.error);
    }
    return;
  }

  // RES_PROBE
  {
    const uint8_t i =
        (r.tankId >= 1 && r.tankId <= relay_client::MAX_TANKS) ? r.tankId - 1 : 0;
    s_tank_online[i] = r.online;
    s_tank_feeding[i] = r.online && r.feeding;
    s_opt_feeding[i] = false; // a real /status result supersedes the optimistic state
    lvgl_port_lock(-1);
    ui_set_probe_busy(false);
    if (i == s_selected_tank) {
      ui_set_relay(r.online, r.ip, r.feeding);
      ui_set_mech(String(r.mech), r.mechRest, r.mechFeed, r.mechHz);
    }
    String ips[relay_client::MAX_TANKS];
    for (uint8_t t = 0; t < relay_client::MAX_TANKS; t++)
      ips[t] = relay_client::ip(t + 1);
    ui_tanks_state(s_tank_online, s_tank_feeding, ips);
    lvgl_port_unlock();
    // Retained MQTT mirror of the relay state (no-ops when disconnected).
    mqtt_mgr::publishTankOnline(r.tankId, r.online);
    recomputeAggregates();
    if (r.report) {
      if (!net_wifi::wifi_is_connected())
        ui_toast("WiFi not connected");
      else if (r.online) {
        String t = String("Tank ") + (int)r.tankId + " OK  v" + r.version;
        if (r.protoMismatch) t += " (protocol mismatch!)";
        ui_toast(t);
      } else
        ui_toast(String("Tank ") + (int)r.tankId + " unreachable");
    }
    return;
  }
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
  ui_set_relay(false, prefs::getTankIp(1));
  ui_set_last_feed(formatLastFed(prefs::getLastFedEpoch()));
  {
    String names[relay_client::MAX_TANKS];
    for (uint8_t i = 0; i < relay_client::MAX_TANKS; i++)
      names[i] = prefs::getTankName(i + 1);
    ui_tanks_rebuild(tankCountClamped(), names, 0);
  }
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

    // scheduled feeding (targets restricted to the configured tanks)
    const FeedTime due = feed_scheduler::tick();
    if (due.enabled) {
      const uint8_t mask = effectiveTankMask(due, tankCountClamped());
      Serial.printf("[FEED] scheduled feed %02u:%02u (%u s, tanks 0x%X)\n",
                    (unsigned)due.hour, (unsigned)due.minute,
                    (unsigned)due.seconds, (unsigned)mask);
      uiToast("Feeding time!");
      requestFeed(due.seconds, false, due.hour, due.minute, false, mask);
    }

    // armed retries for failed scheduled feeds (one delayed attempt each)
    for (SchedRetry &s : s_retries) {
      if (s.pending && (int32_t)(now - s.dueMs) >= 0) {
        s.pending = false;
        Serial.printf("[FEED] retrying scheduled feed %02u:%02u (%u s, tanks 0x%X)\n",
                      (unsigned)s.hour, (unsigned)s.minute,
                      (unsigned)s.seconds, (unsigned)s.tankMask);
        requestFeed(s.seconds, false, s.hour, s.minute, true, s.tankMask);
      }
    }

    // end of the per-tank optimistic feeding indicator (pulse + 2 s elapsed)
    for (uint8_t i = 0; i < tankCountClamped(); i++) {
      if (s_opt_feeding[i] && (int32_t)(now - s_opt_feeding_until[i]) >= 0) {
        s_opt_feeding[i] = false;
        s_tank_feeding[i] = false;
        refreshSelectedTankCard();
      }
    }
    recomputeAggregates();

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

  // ---------------- 60 s block: probe every configured tank (via worker) ----
  if ((now - s_t_60s) >= 60000) {
    s_t_60s = now;
    if (net_wifi::wifi_is_connected()) {
      const uint8_t count = tankCountClamped();
      for (uint8_t t = 1; t <= count; t++) {
        net_worker::enqueueProbe(false, t);
      }
    }
  }

  delay(50);
}
