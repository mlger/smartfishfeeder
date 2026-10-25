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
 * Scheduled-feed resilience (v1.9.0): a failed scheduled feed is retried from
 * a NVS-backed journal (3 retries, 30 s / 5 min / 15 min backoff) that
 * survives power cycles; missed feeds inside the catch-up window are fed on
 * boot/time-sync. On-device manual feeds bypass the policy guards (vacation,
 * min-gap, daily cap) — a human is present.
 *
 * Serial log tags: [FEED] [NET] [UI] [SYS].
 */
#include <Arduino.h>
#include <time.h>

#include <ArduinoOTA.h>

#include "lvgl_v8_port.h"
#include "esp_display_panel.hpp"

#include "feed_scheduler.h"
#include "feed_state.h"
#include "net_wifi.h"
#include "net_worker.h"
#include "mqtt_mgr.h"
#include "prefs.h"
#include "relay_client.h"
#include "web_server.h"
#include "ui/ui.h"
#include "pcf85063.h"
#include "sd_log.h"
#include "net_espnow.h"
#include "net_rf24.h"
#include <catfish_protocol.h>
#include <catfish_mech.h>
#include <catfish_time.h>
#include <esp_sntp.h>

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

#if CATFISH_QEMU
// Smoke-test observability counters (zero cost in hardware builds; incremented
// from requestFeed()'s WiFi-abort branch and asserted by qemuSmokePump()).
static uint32_t g_qemu_wifi_aborts = 0;
#endif

// loop() cadence timestamps
static uint32_t s_t_1s = 0;
static uint32_t s_t_30s = 0;
static uint32_t s_t_60s = 0;
static uint32_t s_t_sd_recheck = 0; // v1.8.4 SD hot-plug recheck cadence

// v1.9.0 (B3) post-feed pulse verification: after a successful feed of a tank
// whose pulse is >= 3 s, one verify probe runs mid-pulse; a relay reporting
// online-but-not-pulsing inside the window surfaces a stall/jam warning.
struct FeedVerify {
  bool armed = false;   // waiting to enqueue the probe
  bool pending = false; // probe enqueued, waiting for the result
  uint32_t dueMs = 0;
  uint32_t untilMs = 0; // result is only meaningful before this
};
static FeedVerify s_verify[relay_client::MAX_TANKS];

// v1.9.0 (A4) catch-up: runs once when the wall clock first becomes valid
// (RTC restore at boot OR the first NTP sync — whichever comes first).
static bool s_catchup_done = false;

// Optimistic "Feeding..." per tank: set right after a successful feed (the
// relay IS running its pulse then), expired after the pulse + 2 s, or
// superseded early by the next real /status probe. Index = tank-1.
static bool s_opt_feeding[relay_client::MAX_TANKS] = {false, false, false, false};
static uint32_t s_opt_feeding_until[relay_client::MAX_TANKS] = {0, 0, 0, 0};

// Last relay state per tank (probe results + optimistic state merged).
static bool s_tank_online[relay_client::MAX_TANKS] = {false, false, false, false};
static bool s_tank_feeding[relay_client::MAX_TANKS] = {false, false, false, false};

// v1.9.0 (B2): "none"/"rtc"/"ntp" — defined after the RTC boot section; the
// web snapshot builder below runs earlier in the file.
static const char *clockSource();

// Selected tank on the Home screen: 0-based index or 0xFE = "All".
static uint8_t s_selected_tank = 0;

// Aggregate state mirrored to MQTT (retained) whenever it changes:
// relay_online = ANY configured tank online; feeding = ANY tank feeding.
static bool s_agg_online_cached = false;
static bool s_agg_feeding_cached = false;

// MQTT status as last painted on the Settings card (0xFF = force first sync).
static uint8_t s_mqtt_last_ui = 0xFF;

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

  char hm[12];
  catfishFormatHm(hm, sizeof(hm), lt.tm_hour, lt.tm_min, prefs::getHour12());
  char buf[40];
  if (ln.tm_year == lt.tm_year && ln.tm_yday == lt.tm_yday)
    snprintf(buf, sizeof(buf), "Today %s", hm);
  else
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %s", lt.tm_year + 1900,
             lt.tm_mon + 1, lt.tm_mday, hm);
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
    ui_set_next_feed_time(nf.hour, nf.minute, nf.daysAhead, true);
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

// ---------------------------------------------------------------------------
// Wireless feed nodes (v1.8.0): ESP-NOW + nRF24L01 status line + helpers
// ---------------------------------------------------------------------------
static uint8_t s_espnow_ui = 0xFF; // last ESP-NOW state painted on the card

static void radiosUiSync() {
  const uint8_t es = (uint8_t)net_espnow::state();
  if (es == s_espnow_ui) return;
  s_espnow_ui = es;
  const char *et = "off";
  switch ((net_espnow::State)es) {
    case net_espnow::State::Disabled:   et = "disabled";          break;
    case net_espnow::State::WaitingWifi: et = "waiting for WiFi"; break;
    case net_espnow::State::Listening:  et = "listening";         break;
    case net_espnow::State::Error:      et = "error";             break;
  }
  const char *rt = "off";
  switch (net_rf24::state()) {
    case net_rf24::State::Disabled:  rt = "disabled"; break;
    case net_rf24::State::Listening: rt = "listening"; break;
    case net_rf24::State::Error:     rt = "not found"; break;
  }
  lvgl_port_lock(-1);
  ui_set_radios(String(et), String(rt));
  lvgl_port_unlock();
}

/** Node keys derive from the GLOBAL pairing token — refresh on rotation. */
static void rfNodeTokenChanged() {
  net_espnow::onTokenChanged();
  net_rf24::onTokenChanged();
}

// --------------------------------------------------------------- v1.10.0
// On-display WiFi manager: mirror the wifi-task's scan/connect lifecycle into
// the Settings WIFI card. All reads are small volatiles + char snapshots.
static constexpr uint8_t WIFI_UI_MAX_APS = 10;
static int s_wifi_scan_ui = 0;   // last scan state painted
static int s_wifi_conn_ui = 0;   // last connect state painted

static void wifiMgrUiSync() {
  const int sc = net_wifi::scanState();
  if (sc != s_wifi_scan_ui) {
    s_wifi_scan_ui = sc;
    if (sc == 1) {
      lvgl_port_lock(-1);
      ui_wifi_scan_busy(true);
      lvgl_port_unlock();
    } else if (sc == 2) {
      uint8_t n = net_wifi::scanCount();
      if (n > WIFI_UI_MAX_APS) n = WIFI_UI_MAX_APS;
      String items[WIFI_UI_MAX_APS];
      for (uint8_t i = 0; i < n; i++) {
        String ssid;
        int rssi = 0;
        bool secure = false;
        if (!net_wifi::scanItem(i, ssid, rssi, secure)) break;
        items[i] = (secure ? String(LV_SYMBOL_SETTINGS) : String("")) + ssid +
                   "   (" + rssi + ")";
      }
      lvgl_port_lock(-1);
      ui_wifi_scan_busy(false);
      ui_wifi_scan_list(n, items);
      lvgl_port_unlock();
    } else if (sc == 3) {
      lvgl_port_lock(-1);
      ui_wifi_scan_busy(false);
      lvgl_port_unlock();
      uiToast("WiFi scan failed");
    }
  }

  const int cc = net_wifi::connectState();
  if (cc != s_wifi_conn_ui) {
    s_wifi_conn_ui = cc;
    if (cc == 2) {
      uiToast(String("WiFi connected: ") + net_wifi::wifi_ssid());
      lvgl_port_lock(-1);
      ui_set_wifi(true, net_wifi::wifi_ssid(), net_wifi::wifi_ip(),
                  net_wifi::wifi_rssi());
      lvgl_port_unlock();
    } else if (cc == 3) {
      uiToast("WiFi connect failed (check password)");
    }
  }
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
  mqtt_mgr::publishPaused(prefs::getVacation()); // v1.9.0 (A5)
  mqtt_mgr::publishFeedsToday(feed_state::feedsOkToday(),
                              feed_state::feedsFailToday()); // v1.9.0 (C3)
  mqtt_mgr::publishFoodLevels(); // v1.9.0 (D1)
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
  s.defGrams = prefs::getDefaultGrams(); // v1.10.0: default portion (g)
  s.schedCount = (uint16_t)feed_scheduler::getCount();
  s.clockValid = feed_scheduler::timeValid();
  s.hour12 = prefs::getHour12();
  // v1.9.0: feeding state + policy for the dashboard
  s.vacation = prefs::getVacation();
  s.feedsOkToday = feed_state::feedsOkToday();
  s.feedsFailToday = feed_state::feedsFailToday();
  s.totalFeeds = feed_state::totalFeeds();
  strlcpy(s.clockSrc, clockSource(), sizeof(s.clockSrc));
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
    s.tankFoodPct[i] = feed_state::hopperLevelPct(i + 1); // 255 = unknown
    s.tankGps[i] = prefs::getTankGps(i + 1);
  }
  // v1.9.0 (C1/C2): history JSON for /api/history + the dashboard — rebuilt
  // only when the ring changed (feed events are rare), then mirrored to the
  // on-device history overlay.
  static bool s_hist_pushed = false;
  if (feed_state::historyDirty() || !s_hist_pushed) {
    const String hj = feed_state::historyJson(24);
    strlcpy(s.historyJson, hj.c_str(), sizeof(s.historyJson));
    feed_state::historyClearDirty();
    s_hist_pushed = true;
    lvgl_port_lock(-1);
    ui_set_history(hj);
    lvgl_port_unlock();
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

// ---------------------------------------------------------------------------
// Battery-backed RTC (v1.6.0): the PCF85063 + CR927 cell keeps wall time while
// main power is off. Boot: if the chip holds a plausible time, restore it via
// settimeofday() so schedules/last-fed are correct immediately (even offline).
// Once NTP actually syncs, the wall clock is written back into the chip.
// v1.8.1: probing happens AFTER Board::begin() — the panel stack owns the
// legacy I2C driver on host 0, and nothing can talk I2C before it is up.
// ---------------------------------------------------------------------------
static bool s_rtc_present = false;
static bool s_rtc_ntp_synced = false;
static String s_rtc_status = "not present";
static uint32_t s_rtc_last_write_ms = 0; // v1.8.4 hourly drift-correction rewrite

// v1.9.0 (B2): which time source the valid wall clock came from
// ("none" / "rtc" / "ntp") for /api/status + the Settings RTC line.
static const char *clockSource() {
  if (!feed_scheduler::timeValid()) return "none";
  return s_rtc_ntp_synced ? "ntp" : "rtc";
}

// "Plausible" floor: 2025-01-01 UTC, matching feed_scheduler::timeValid().
// An RTC older than this was never set (fresh board / battery just added) —
// don't restore garbage (v1.8.2: was 2024, which restored a clock the
// scheduler still refused to trust).
static const time_t CATFISH_RTC_MIN_EPOCH = 1735689600;

static void rtcBootInit() {
  s_rtc_present = pcf85063::begin();
  if (!s_rtc_present) {
    s_rtc_status = "not present";
    Serial.println("[SYS] WARN: PCF85063 RTC not found (CR927 cell / holder?)");
    return;
  }
  time_t rtcEpoch = 0;
  bool oscStopped = false;
  if (!pcf85063::readUtc(&rtcEpoch, &oscStopped)) {
    s_rtc_status = "read error";
    Serial.println("[SYS] WARN: RTC present but registers unreadable");
    return;
  }
  if (oscStopped) {
    s_rtc_status = "battery dead/absent";
    Serial.println("[SYS] WARN: RTC oscillator stopped - replace the CR927 cell");
    return;
  }
  if (rtcEpoch < CATFISH_RTC_MIN_EPOCH) {
    s_rtc_status = "time never set";
    Serial.println("[SYS] RTC present but implausible - waiting for NTP");
    return;
  }
  struct timeval tv;
  tv.tv_sec = rtcEpoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  s_rtc_status = "battery OK";
  Serial.printf("[SYS] RTC restore: epoch %lld (battery OK)\n",
                (long long)rtcEpoch);
}

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

/**
 * Route a feed request to the worker. Manual feeds get immediate feedback.
 * v1.10.0: @p grams is the requested portion; per-tank wire pulses (seconds)
 * are derived here from the tank's calibrated g/s rate (per-tank gram
 * overrides win). v1.9.0: @p src drives the F3/A5 policy gate (vacation /
 * min-gap / daily cap — on-device touch always passes); scheduled + catch-up
 * feeds mark the fired-map and arm the persistent retry journal on failure.
 */
static void requestFeed(uint16_t grams, bool manual, uint8_t hour,
                        uint8_t minute, bool retry, uint8_t tankMask,
                        const uint16_t *gramTank, feed_state::FeedSource src) {
  const uint32_t nowEpoch = (uint32_t)time(nullptr);

  // F3/A5 policy gate — retries re-check too (vacation may have flipped on
  // since the original attempt; a blocked retry gives up instead of looping).
  const feed_state::GuardResult g = feed_state::checkFeed(src, nowEpoch);
  if (!g.allowed) {
    if (retry) feed_state::journalClearFor(hour, minute);
    uiToast(String("Feed blocked (") + g.reason + ")");
    feed_state::recordHistory(tankMask ? tankMask : 0x01, 0, grams, false,
                              feed_state::HIST_BLOCKED);
    Serial.printf("[FEED] blocked by policy (%s, src %u)\n", g.reason,
                  (unsigned)src);
    return;
  }

  // v1.10.0: per-tank wire pulse = grams / tank gps (ceil, clamped).
  uint8_t secPerTank[4] = {0, 0, 0, 0};
  for (uint8_t t = 1; t <= relay_client::MAX_TANKS; t++) {
    if (tankMask && !(tankMask & (1u << (t - 1)))) continue;
    const uint16_t gt =
        (gramTank && gramTank[t - 1]) ? gramTank[t - 1] : grams;
    secPerTank[t - 1] = catfishGramsToSeconds(gt, prefs::getTankGps(t));
  }

  if (!net_wifi::wifi_is_connected()) {
    if (manual) {
      uiToast("WiFi not connected");
    } else if (!retry) {
      // H2: a scheduled feed must survive a WiFi blip at fire time - arm a
      // delayed attempt in the PERSISTENT journal (v1.9.0) instead of
      // silently dropping it.
      if (feed_state::journalArm(hour, minute, grams, tankMask)) {
        uiToast("No WiFi - feed will retry");
      } else {
        uiToast(String("Feed FAILED: no WiFi"));
      }
    } else {
      // retry attempt aborted: resolve the in-flight slot (attempts++ /
      // eventual give-up) so it cannot linger forever.
      feed_state::journalFail(hour, minute);
      uiToast(String("Feed FAILED: no WiFi"));
    }
    Serial.println("[FEED] aborted: WiFi down");
#if CATFISH_QEMU
    g_qemu_wifi_aborts++;
#endif
    return;
  }
  if (!net_worker::enqueueFeed(grams, secPerTank, manual, hour, minute, retry,
                               tankMask, src == feed_state::SRC_CATCHUP)) {
    // v1.8.2: a scheduled feed dropped because the worker queue was full gets
    // a journal slot instead of being lost with a misleading "try again" toast.
    if (!manual && !retry) {
      feed_state::journalArm(hour, minute, grams, tankMask);
      uiToast("Feeder busy - feed will retry");
      Serial.println("[FEED] worker queue full, scheduled feed retry armed");
    } else if (!manual && retry) {
      // defer the in-flight retry by re-arming it (attempt count preserved)
      feed_state::journalArm(hour, minute, grams, tankMask);
      uiToast("Feeder busy - retry deferred");
    } else {
      uiToast("Too busy, try again");
      Serial.println("[FEED] worker queue full, request dropped");
    }
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

/** v1.9.0 (D1): push the hopper estimator into the UI (Food card + Home bar). */
static void refreshFoodUi() {
  uint8_t pct[relay_client::MAX_TANKS];
  uint16_t used[relay_client::MAX_TANKS];
  uint16_t gps[relay_client::MAX_TANKS];
  uint16_t cap[relay_client::MAX_TANKS];
  for (uint8_t t = 1; t <= relay_client::MAX_TANKS; t++) {
    pct[t - 1] = feed_state::hopperLevelPct(t);
    used[t - 1] = feed_state::hopperUsedG(t); // v1.10.0: grams
    gps[t - 1] = prefs::getTankGps(t);
    cap[t - 1] = prefs::getTankHopperCap(t); // v1.10.0: grams
  }
  lvgl_port_lock(-1);
  ui_set_food(tankCountClamped(), pct, used, gps, cap, prefs::getHopperLowPct());
  lvgl_port_unlock();
}

/**
 * v1.9.0 (A4): feed entries missed while the unit was off/unreachable, inside
 * the configured catch-up window. Runs once when the wall clock first becomes
 * valid (RTC boot restore OR first NTP sync). Each due entry re-checks the
 * policy guards and carries the entry's per-tank seconds (A2).
 */
static void runCatchupScan() {
  const uint8_t window = prefs::getCatchupWin();
  feed_state::CatchupDue due[relay_client::MAX_TANKS];
  const int n = feed_state::scanCatchup(feed_scheduler::getAll(),
                                        feed_scheduler::getCount(),
                                        (uint32_t)time(nullptr), window, due,
                                        relay_client::MAX_TANKS);
  if (n == 0) return;
  const int cnt = feed_scheduler::getCount();
  const FeedTime *all = feed_scheduler::getAll();
  for (int i = 0; i < n; i++) {
    const FeedTime *e = nullptr;
    for (int j = 0; j < cnt; j++) {
      if (all[j].hour == due[i].hour && all[j].minute == due[i].minute) {
        e = &all[j];
        break;
      }
    }
    uint16_t gt[4] = {0, 0, 0, 0};
    uint8_t mask = due[i].tankMask;
    if (e) {
      mask = effectiveTankMask(*e, tankCountClamped());
      for (uint8_t t = 1; t <= relay_client::MAX_TANKS; t++)
        gt[t - 1] = entryGramsForTank(*e, t); // v1.10.0: per-tank grams
    }
    // Guards re-checked: vacation may have been enabled while away.
    const feed_state::GuardResult g = feed_state::checkFeed(
        feed_state::SRC_CATCHUP, (uint32_t)time(nullptr));
    if (!g.allowed) {
      feed_state::recordHistory(mask, 0, due[i].grams, false,
                                feed_state::HIST_BLOCKED);
      uiToast(String("Catch-up blocked (") + g.reason + ")");
      continue;
    }
    uiToast(String("Catch-up feed ") + fmtHhmm(due[i].hour, due[i].minute));
    Serial.printf("[FEED] catch-up firing %02u:%02u (%u g, tanks 0x%X)\n",
                  (unsigned)due[i].hour, (unsigned)due[i].minute,
                  (unsigned)due[i].grams, (unsigned)mask);
    requestFeed(due[i].grams, false, due[i].hour, due[i].minute, false, mask,
                gt, feed_state::SRC_CATCHUP);
  }
}

// ---------------------------------------------------------------------------
// UI event handling (loop context; never blocks except in-event flash writes)
// ---------------------------------------------------------------------------
static void handleUiEvent(const UiEvent &ev) {
  switch (ev.type) {
    case UI_EVT_FEED: {
      Serial.printf("[FEED] manual feed %u g (mask 0x%X, src %u)\n",
                    (unsigned)ev.grams, (unsigned)ev.tankMask,
                    (unsigned)ev.feedSrc);
      // explicit mask (MQTT tank button / dashboard) or the Home selection
      const uint8_t mask = ev.tankMask ? ev.tankMask
                                       : (s_selected_tank == 0xFE
                                              ? configuredTankMask()
                                              : (uint8_t)(1u << s_selected_tank));
      // v1.9.0 (mapping FIXED in v1.10.0): web AND MQTT feeds are REMOTE for
      // the policy gate; touch and the wireless node buttons are TOUCH (a
      // human is physically present). The old `== 1` check let MQTT feeds
      // (feedSrc 2) bypass vacation / min-gap / the daily cap.
      const feed_state::FeedSource src =
          (ev.feedSrc != 0) ? feed_state::SRC_REMOTE : feed_state::SRC_TOUCH;
      requestFeed(ev.grams, true, 0, 0, false, mask, ev.gramTank, src);
      break;
    }

    case UI_EVT_VACATION:
      // v1.9.0 (A5): pause scheduled/catch-up/remote feeding; on-device
      // manual feeds stay live (a human is present).
      prefs::setVacation(ev.flag);
      lvgl_port_lock(-1);
      ui_set_vacation(ev.flag);
      lvgl_port_unlock();
      mqtt_mgr::publishPaused(ev.flag); // retained; no-op when disconnected
      uiToast(ev.flag ? "Vacation ON - schedules paused"
                      : "Vacation OFF - schedules live");
      Serial.printf("[FEED] vacation mode %s\n", ev.flag ? "ON" : "OFF");
      break;

    case UI_EVT_SKIP_NEXT:
      // v1.9.0 (E4): skip the next occurrence of one entry exactly once.
      feed_state::setSkipNext(ev.a, ev.b);
      uiToast(String("Will skip ") + fmtHhmm(ev.a, ev.b) + " once");
      break;

    case UI_EVT_REFILL:
      // v1.9.0 (D1): reset the hopper estimator (one tank or all).
      feed_state::hopperRefill(ev.a);
      uiToast(ev.a == 0xFF ? "All hoppers refilled"
                           : (String("Tank ") + (int)ev.a + " refilled"));
      refreshFoodUi();
      mqtt_mgr::publishFoodLevels();
      Serial.printf("[FEED] hopper refill (tank 0x%02X)\n", (unsigned)ev.a);
      break;

    case UI_EVT_GUARD_CFG:
      // v1.9.0 (F3/D1): feeding-policy card saved (single writer persists all).
      prefs::setMinGapMin(ev.a);
      prefs::setMaxFeedsDay(ev.b);
      prefs::setCatchupWin(ev.c);
      prefs::setHopperLowPct(ev.d);
      uiToast("Feeding policy saved");
      refreshFoodUi();
      Serial.printf("[FEED] policy saved: gap %u min, max/day %u, catchup %u min, low %u%%\n",
                    (unsigned)ev.a, (unsigned)ev.b, (unsigned)ev.c, (unsigned)ev.d);
      break;

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

    case UI_EVT_SET_PORTION:
      // v1.10.0: the Home portion spinbox is GRAMS (persisted as defG).
      if (ev.grams == prefs::getDefaultGrams())
        break; // no-op replay guard (v1.8.3): skip NVS write + repaint
      prefs::setDefaultGrams(ev.grams);
      lvgl_port_lock(-1);
      ui_set_portion(ev.grams); // also corrects the spinbox when set remotely
      lvgl_port_unlock();
      Serial.printf("[FEED] default portion set to %u g\n", (unsigned)ev.grams);
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
        rfNodeTokenChanged(); // node packets re-key against the global token
        uiToast("Pairing token updated");
      }
      Serial.println("[NET] pairing token update requested");
      break;

    case UI_EVT_SET_BL:
      prefs::setBacklightOn(ev.flag);
      Serial.printf("[SYS] backlight preference saved: %s\n",
                    ev.flag ? "on" : "off");
      break;

    case UI_EVT_SET_FMT12: {
      // v1.6.0: clock presentation toggle. Persists here (single writer), the
      // UI re-renders its cached labels; MQTT/HTTP stay 24-hour by contract.
      prefs::setHour12(ev.flag);
      lvgl_port_lock(-1);
      ui_set_hour12(ev.flag);
      ui_set_last_feed(formatLastFed(prefs::getLastFedEpoch()));
      lvgl_port_unlock();
      refreshNextFeedUi();
      Serial.printf("[SYS] clock format saved: %s\n", ev.flag ? "12-hour" : "24-hour");
      break;
    }

    case UI_EVT_SCHED_ADD: {
      const bool full =
          feed_scheduler::getCount() >= feed_scheduler::MAX_FEED_TIMES;
      FeedTime t{};
      t.hour = ev.a;
      t.minute = ev.b;
      t.enabled = true;
      t.grams = ev.grams; // v1.10.0: portion in grams
      t.tanks = ev.tankMask;
      t.days = ev.days; // v1.9.0 (A1): weekday mask (0 = clamp to daily)
      for (int k = 0; k < 4; k++) t.gramTank[k] = ev.gramTank[k]; // A2 (grams)
      if (feed_scheduler::add(t)) {
        afterScheduleChange(false);
      } else {
        uiToast(full ? "Schedule full (max 8)" : "Time already scheduled");
        Serial.println("[FEED] schedule add rejected");
      }
      break;
    }

    case UI_EVT_SCHED_REMOVE:
      // v1.10.0: identity is HH:MM (ev.a = h, ev.b = m) — duplicates are
      // rejected on add, so the pair is unique.
      if (feed_scheduler::removeByIdentity(ev.a, ev.b)) {
        feed_state::pruneFired(feed_scheduler::getAll(),
                               feed_scheduler::getCount());
        afterScheduleChange(true);
      } else {
        Serial.println("[FEED] schedule remove: entry not found");
      }
      break;

    case UI_EVT_SCHED_PERSIST:
      // v1.10.0: identity-packed (h in a, m in b) so a queued toggle survives
      // index shifts from a dashboard replaceAll or a same-batch trash tap.
      feed_scheduler::setEnabledByIdentity(ev.a, ev.b, ev.flag);
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
      if ((uint8_t)ev.a == prev)
        break; // no-op replay guard (v1.8.3): skip NVS write + rebuild storm
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
      } else if (nw > prev) {
        // v1.8.2: grow — publish HA discovery for the new tanks right away
        // instead of waiting for the next broker reconnect.
        mqtt_mgr::publishDiscovery();
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
      // v1.9.0 (A3/D1): per-tank food rate + hopper capacity
      prefs::setTankGps(ev.a, ev.tankGps);
      prefs::setTankHopperCap(ev.a, ev.tankHcap);
      relay_client::applyTankTokens(); // token override may have changed
      syncTanksUi();
      refreshFoodUi();
      uiToast("Tank updated");
      Serial.printf("[SYS] tank %u config saved (name '%s', %u g/s, cap %u s)\n",
                    (unsigned)ev.a, ev.tankName, (unsigned)ev.tankGps,
                    (unsigned)ev.tankHcap);
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

    case UI_EVT_RF_FEED: {
      // v1.8.0: wireless node (ESP-NOW / nRF24) pressed its feed button.
      // Routes through the same manual-feed pipeline as touch/MQTT/dashboard.
      const uint8_t mask =
          ev.tankMask ? ev.tankMask
                      : (s_selected_tank == 0xFE
                             ? configuredTankMask()
                             : (uint8_t)(1u << s_selected_tank));
      // v1.10.0: producers convert the datagram's seconds to grams already;
      // fall back to the default portion for anything older.
      const uint16_t grams =
          ev.grams ? ev.grams
                   : catfishSecondsToGrams(ev.a, prefs::getTankGps(1));
      Serial.printf("[RF] node feed %u g (mask 0x%X)\n", (unsigned)grams,
                    (unsigned)mask);
      requestFeed(grams, true, 0, 0, false, mask, nullptr,
                  feed_state::SRC_TOUCH);
      break;
    }

    case UI_EVT_WIFI_SCAN:
      // v1.10.0: on-display WiFi manager — async scan handled by wifi-task.
      net_wifi::requestScan();
      break;

    case UI_EVT_WIFI_CONNECT:
      // v1.10.0: connect to a user-picked AP (runs on wifi-task, not here).
      net_wifi::requestConnect(String(ev.wifiSsid), String(ev.wifiPass));
      uiToast(String("Connecting to ") + ev.wifiSsid + "...");
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
      relay_client::applyTankTokens(); // v1.8.2: refresh the worker's token
                                       // snapshot, or the next feed 403s
      rfNodeTokenChanged(); // rotation changed the global token -> re-key nodes
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
    const bool clock_ok = feed_scheduler::timeValid();
    const uint32_t nowEpoch = clock_ok ? (uint32_t)time(nullptr) : 0;
    const uint8_t hflag = r.catchup ? feed_state::HIST_CATCHUP
                          : r.retry ? feed_state::HIST_RETRY
                          : r.manual ? feed_state::HIST_MANUAL
                                     : feed_state::HIST_NONE;

    // v1.7.0: failed feeds land in the SD history too (when the clock is valid)
    // v1.9.0: ...and in the on-device history ring.
    if (!r.ok) {
      if (clock_ok) {
        sd_log::logFeed(nowEpoch, (uint8_t)(r.tanksFail ? r.tanksFail : 0x01),
                        r.seconds, false);
      }
      feed_state::recordHistory((uint8_t)(r.tanksFail ? r.tanksFail : 0x01),
                                r.seconds, r.grams, false, hflag);
      feed_state::onFeedResult(false, r.seconds); // C3
    }
    if (r.ok) {
      if (!r.manual) feed_state::journalClearFor(r.hour, r.minute); // B1

      String when;
      if (clock_ok) {
        prefs::setLastFedEpoch(nowEpoch);
        sd_log::logFeed(nowEpoch, (uint8_t)(r.tanksOk ? r.tanksOk : 0x01),
                        r.seconds, true);
        feed_state::recordHistory((uint8_t)(r.tanksOk ? r.tanksOk : 0x01),
                                  r.seconds, r.grams, true, hflag);
        when = r.manual ? formatLastFed(nowEpoch)
                        : (String("Today ") + fmtHhmm(r.hour, r.minute));
      }
      feed_state::onFeedResult(true, r.seconds); // C3 counters
      // v1.10.0 (D1): hopper usage in grams — derived from the applied wire
      // pulses and each tank's calibrated g/s rate.
      uint16_t usedG[4] = {0, 0, 0, 0};
      for (uint8_t t = 1; t <= relay_client::MAX_TANKS; t++) {
        if (r.tanksOk & (1u << (t - 1)))
          usedG[t - 1] = catfishSecondsToGrams(r.secOk[t - 1],
                                               prefs::getTankGps(t));
      }
      feed_state::hopperAddUsage(r.tanksOk, usedG);

      // Optimistic feeding state per successfully fed tank.
      for (uint8_t t = 1; t <= relay_client::MAX_TANKS; t++) {
        if (r.tanksOk & (1u << (t - 1))) {
          s_tank_feeding[t - 1] = true;
          s_opt_feeding[t - 1] = true;
          s_opt_feeding_until[t - 1] =
              millis() + (uint32_t)r.seconds * 1000UL + 2000UL;
          // v1.9.0 (B3): arm a mid-pulse verify probe for pulses >= 3 s
          if (r.secOk[t - 1] >= 3) {
            s_verify[t - 1].armed = true;
            s_verify[t - 1].pending = false;
            s_verify[t - 1].dueMs = millis() + 2500;
            s_verify[t - 1].untilMs = millis() + 15000;
          }
        }
      }

      lvgl_port_lock(-1);
      ui_set_feed_busy(false);
      if (clock_ok) ui_set_last_feed(when);
      if (r.manual) {
        String t = String("Fed OK (") + r.grams + " g";
        if (r.tanksOk & (r.tanksOk - 1)) t += ", multiple tanks";
        if (r.clamped) t += " - clamped"; // M3: relay clamp surfaced
        t += ")";
        ui_toast(t);
      } else {
        String t = String(r.catchup ? "Catch-up fed " : "Fed ") +
                   fmtHhmm(r.hour, r.minute);
        if (r.tanksFail) t += " (some tanks failed)";
        ui_toast(t);
      }
      lvgl_port_unlock();
      refreshSelectedTankCard();

      if (r.clamped)
        Serial.printf("[FEED] relay clamp note: %s\n", r.note);

      // v1.9.0 (F2): per-tank last-feed mirror (retained).
      if (clock_ok) {
        for (uint8_t t = 1; t <= relay_client::MAX_TANKS; t++) {
          if (r.tanksOk & (1u << (t - 1)))
            mqtt_mgr::publishTankLastFeed(t, nowEpoch, r.secOk[t - 1]);
        }
      }
      mqtt_mgr::publishFeedsToday(feed_state::feedsOkToday(),
                                  feed_state::feedsFailToday()); // C3
      mqtt_mgr::publishFoodLevels(); // D1 (usage changed)

      if (r.tanksFail) {
        Serial.printf("[FEED] partial feed: ok=0x%X fail=0x%X (%s)\n",
                      (unsigned)r.tanksOk, (unsigned)r.tanksFail, r.error);
        // v1.8.2/v1.9.0: partial scheduled feeds retry the failed tanks via
        // the persistent journal — the success of tank 1 must not strand
        // tank 2 (P5 intent).
        if (!r.manual && !r.retry) {
          if (feed_state::journalArm(r.hour, r.minute, r.grams,
                                     r.tanksFail)) {
            uiToast("Some tanks failed - retrying in 30 s");
            Serial.printf("[FEED] partial scheduled feed, retry armed (mask 0x%X)\n",
                          (unsigned)r.tanksFail);
          }
        } else if (!r.manual && r.retry) {
          if (feed_state::journalFail(r.hour, r.minute))
            uiToast("Some tanks failed - retrying later");
        }
      }
      // Retained MQTT mirror: the relay IS pulsing now (M2-consistent).
      recomputeAggregates();
      if (clock_ok) mqtt_mgr::publishLastFeed(prefs::getLastFedEpoch());
      Serial.printf("[FEED] feed OK via %s (%u g / %u s, tanks ok=0x%X)\n", r.ip,
                    (unsigned)r.grams, (unsigned)r.seconds, (unsigned)r.tanksOk);
      refreshNextFeedUi();
      refreshFoodUi();
    } else if (r.manual) {
      lvgl_port_lock(-1);
      ui_set_feed_busy(false);
      ui_toast(String("Feed failed: ") + r.error);
      lvgl_port_unlock();
      Serial.printf("[FEED] manual feed FAILED: %s\n", r.error);
      mqtt_mgr::publishFeedsToday(feed_state::feedsOkToday(),
                                  feed_state::feedsFailToday());
    } else {
      // scheduled feed (all target tanks failed): persistent-journal retry
      // (the failure was already counted/recorded in the !r.ok branch above)
      if (r.retry) {
        const bool more = feed_state::journalFail(r.hour, r.minute);
        uiToast(more ? String("Feed failed - retrying later")
                     : (String("Feed FAILED: ") + r.error));
        Serial.printf("[FEED] scheduled feed FAILED (%s) - %s\n", r.error,
                      more ? "another retry armed" : "journal gave up");
      } else if (feed_state::journalArm(r.hour, r.minute, r.grams,
                                        r.tanksFail ? r.tanksFail : 0x01)) {
        uiToast("Feed failed - retrying in 30 s");
        Serial.printf("[FEED] scheduled feed failed (%s), retry armed\n",
                      r.error);
      } else {
        uiToast(String("Feed FAILED: ") + r.error);
        Serial.println("[FEED] scheduled feed FAILED (retry slots full)");
      }
      mqtt_mgr::publishFeedsToday(feed_state::feedsOkToday(),
                                  feed_state::feedsFailToday());
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
    // v1.9.0 (B3): post-feed pulse verification result
    if (r.verify) {
      s_verify[i].pending = false;
      if (r.online && !r.feeding && (int32_t)(millis() - s_verify[i].untilMs) < 0) {
        const uint8_t bit = (uint8_t)(1u << i);
        feed_state::recordHistory(bit, 0, 0, false, feed_state::HIST_VERIFY);
        uiToast(String("Tank ") + (int)r.tankId + " not pulsing? (check food)"
                );
        Serial.printf("[FEED] VERIFY FAIL tank %u: relay online but not feeding "
                      "inside the pulse window (jam / empty hopper?)\n",
                      (unsigned)r.tankId);
      } else if (r.online && r.feeding) {
        Serial.printf("[FEED] verify OK tank %u (pulsing)\n", (unsigned)r.tankId);
      }
    }
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
    refreshSelectedTankCard(); // v1.8.2: keep the "All" card fresh after probes too
    if (r.report) {
      if (!net_wifi::wifi_is_connected())
        uiToast("WiFi not connected");
      else if (r.online) {
        String t = String("Tank ") + (int)r.tankId + " OK  v" + r.version;
        if (r.protoMismatch) t += " (protocol mismatch!)";
        uiToast(t);
      } else
        uiToast(String("Tank ") + (int)r.tankId + " unreachable");
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// QEMU smoke test (compiled only with -DCATFISH_QEMU=1; run by
// scripts/qemu_run.sh and the CI "QEMU smoke test" job, see docs/tooling.md).
// Scripts a fixed sequence of real UiEvents through the SAME queue the touch
// UI uses, then verifies each side effect (NVS persist, scheduler state,
// WiFi-down abort accounting). Verdict lines are grepped by the runner: any
// "[QEMU] FAIL", or a missing "[QEMU] SMOKE PASS", fails the run.
// ---------------------------------------------------------------------------
#if CATFISH_QEMU
static void qemuSmokeLog(const char *what, bool ok) {
  Serial.printf("[QEMU] %s %s\n", ok ? "PASS" : "FAIL", what);
}

static void qemuSmokeSend(const UiEvent &ev) {
  Serial.printf("[QEMU] inject event %d\n", (int)ev.type);
  if (!ui_event_send(ev)) Serial.println("[QEMU] FAIL event queue send");
}

static void qemuSmokePump() {
  static uint8_t s_step = 0;
  static uint32_t s_due = 2500; // let setup() and the first 1 Hz ticks settle
  if ((int32_t)(millis() - s_due) < 0) return;
  s_due = millis() + 1200;

  switch (s_step) {
    case 0: {
      UiEvent ev{}; // default portion -> 7 g
      ev.type = UI_EVT_SET_PORTION;
      ev.grams = 7;
      qemuSmokeSend(ev);
      s_step = 1;
      break;
    }
    case 1:
      qemuSmokeLog("SET_PORTION persisted (NVS)", prefs::getDefaultGrams() == 7);
      {
        UiEvent ev{}; // add schedule 08:00, 15 g, tank 1
        ev.type = UI_EVT_SCHED_ADD;
        ev.a = 8;
        ev.b = 0;
        ev.grams = 15;
        ev.tankMask = 1;
        qemuSmokeSend(ev);
      }
      s_step = 2;
      break;
    case 2: {
      const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
      Serial.printf("[QEMU] next feed %02u:%02u valid=%u (count=%u)\n",
                    (unsigned)nf.hour, (unsigned)nf.minute, nf.valid ? 1U : 0U,
                    (unsigned)feed_scheduler::getCount());
      // 2 first-boot seeded entries + the added 08:00; exact next-occurrence
      // math is covered by the native test suite — here we prove the wiring.
      qemuSmokeLog("SCHED_ADD accepted (2 seeded + 1 new)",
                   feed_scheduler::getCount() == 3 && nf.valid);
      UiEvent ev{}; // manual feed -> WiFi is down on purpose
      ev.type = UI_EVT_FEED;
      ev.a = 3;
      ev.tankMask = 1;
      qemuSmokeSend(ev);
      s_step = 3;
      break;
    }
    case 3:
      qemuSmokeLog("FEED aborts cleanly with no WiFi",
                   g_qemu_wifi_aborts == 1);
      {
        UiEvent ev{}; // 12-hour clock toggle
        ev.type = UI_EVT_SET_FMT12;
        ev.flag = true;
        qemuSmokeSend(ev);
      }
      s_step = 4;
      break;
    case 4:
      qemuSmokeLog("SET_FMT12 persisted", prefs::getHour12());
      {
        UiEvent ev{}; // Home "All tanks" chip
        ev.type = UI_EVT_SELECT_TANK;
        ev.a = 0xFE;
        qemuSmokeSend(ev);
      }
      s_step = 5;
      break;
    case 5:
      qemuSmokeLog("SELECT_TANK(All) routed", s_selected_tank == 0xFE);
      {
        UiEvent ev{}; // v1.9.0 (A5): vacation toggle ON
        ev.type = UI_EVT_VACATION;
        ev.flag = true;
        qemuSmokeSend(ev);
      }
      s_step = 6;
      break;
    case 6:
      qemuSmokeLog("VACATION persisted", prefs::getVacation());
      {
        UiEvent ev{}; // v1.9.0 (E4): skip-next marker for 07:30
        ev.type = UI_EVT_SKIP_NEXT;
        ev.a = 7;
        ev.b = 30;
        qemuSmokeSend(ev);
      }
      s_step = 7;
      break;
    case 7:
      qemuSmokeLog("SKIP_NEXT armed + consumed once", feed_state::consumeSkip(7, 30) &&
                                                             !feed_state::consumeSkip(7, 30));
      {
        UiEvent ev{}; // vacation back OFF (leave clean state)
        ev.type = UI_EVT_VACATION;
        ev.flag = false;
        qemuSmokeSend(ev);
      }
      s_step = 8;
      break;
    case 8:
      qemuSmokeLog("VACATION cleared", !prefs::getVacation());
      Serial.println("[QEMU] SMOKE PASS");
      s_step = 9;
      break;
    default: {
      // Steady-state heartbeat: keep running (dim timers, LVGL rebuilds,
      // 1 Hz housekeeping) so late crashes still fail the run.
      static uint32_t s_beat = 0;
      if (millis() - s_beat >= 30000) {
        s_beat = millis();
        Serial.printf("[QEMU] alive %lus\n", (unsigned long)(s_beat / 1000));
      }
      break;
    }
  }
}
#endif // CATFISH_QEMU

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
  feed_state::init(); // v1.9.0: journal / fired-map / counters / hopper
  relay_client::init();
  if (!mqtt_mgr::begin()) {
    Serial.println("[SYS] WARN: MQTT manager unavailable (continuing without)");
  }
  if (!web_server::begin()) {
    Serial.println("[SYS] WARN: web dashboard unavailable (continuing without)");
  }

  // --- board + LVGL ---
  // The panel stack installs + owns the LEGACY I2C driver on host 0 inside
  // begin() (board conf *_SKIP_INIT_HOST = 0). Nothing may talk I2C before
  // this point — GT911, CH422G (LCD_RST + backlight!) and the RTC all ride
  // that driver. See esp_panel_board_custom_conf.h for the v1.8.1 rationale.
#if CATFISH_QEMU
  // Headless QEMU build: the emulator has no panel/expander/I2C/SPI hardware.
  // A fixed wall clock is injected instead of the RTC restore so every
  // time-gated path (scheduler, countdown, last-fed) behaves exactly like
  // after an NTP sync: 2025-02-07 07:45:00 UTC — safely BETWEEN the two
  // first-boot seeded schedule entries (07:30, 17:30) so neither fires while
  // the smoke sequence runs.
  {
    struct timeval tv = {};
    tv.tv_sec = CATFISH_RTC_MIN_EPOCH + (time_t)(37 * 86400) +
                (time_t)(7 * 3600 + 45 * 60);
    settimeofday(&tv, nullptr);
    s_rtc_status = "qemu injected";
    Serial.printf("[SYS] QEMU: injected wall clock epoch %lld\n",
                  (long long)tv.tv_sec);
  }
#else
  s_board = new Board();
  s_board->init();
  // v1.8.2 fix (flicker): avoid-tearing mode 3 needs TWO full frame buffers,
  // but the RGB bus defaults to num_fbs=1 — the vendor port header explicitly
  // says the app must request the buffer count BEFORE begin(). Without this,
  // getFrameBufferByIndex(1) returned nullptr and LVGL rendered straight into
  // the buffer the RGB peripheral was scanning (tearing on every partial
  // update). Costs one extra 768 KB PSRAM buffer (8 MB available).
  if (s_board->getLCD()) {
    s_board->getLCD()->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
  }
  if (!s_board->begin()) {
    Serial.println("[SYS] ERROR: board begin() failed - halting");
    while (true) delay(1000);
  }
  if (s_board->getBacklight()) {
    s_board->getBacklight()->setBrightness(1); // switch type: on (no PWM)
  }

  // --- battery-backed RTC (v1.8.1: after the panel stack owns the bus) ---
  // Probing earlier would fail (no legacy driver installed yet). Ordering
  // still keeps the restored wall clock in effect before the first LVGL
  // frame renders (lvgl_port_init/ui_init come below).
  rtcBootInit();

  // --- optional microSD feed log (v1.7.0) ---
  // After board begin(): the CH422G expander (TF-card CS) is up. Blocking
  // mount is fine here (setup context); a missing card just disables it.
  sd_log::begin(s_board->getIO_Expander() ? s_board->getIO_Expander()->getBase()
                                          : nullptr);

  // --- optional wireless feed nodes (v1.8.0) ---
  // nRF24 polls on its own task (SPI shared with the SD card via the bus
  // guard); ESP-NOW starts lazily once WiFi is up (loop()).
  net_rf24::begin();
#endif

  if (!lvgl_port_init(
#if CATFISH_QEMU
          nullptr, nullptr
#else
          s_board->getLCD(), s_board->getTouch()
#endif
          )) {
    Serial.println("[SYS] ERROR: LVGL port init failed");
  }

  // build UI under lock
  lvgl_port_lock(-1);
  ui_init();
  ui_set_backlight_cb(appSetBacklight, prefs::getBacklightOn());
  ui_set_hour12(prefs::getHour12());
  ui_set_rtc(s_rtc_status);
  ui_set_sd(sd_log::summary());
  radiosUiSync();
  ui_set_relay(false, prefs::getTankIp(1));
  ui_set_last_feed(formatLastFed(prefs::getLastFedEpoch()));
  ui_set_vacation(prefs::getVacation()); // v1.9.0 (A5) Home badge
  refreshFoodUi();                        // v1.9.0 (D1) Food card + Home bar
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
#if CATFISH_QEMU
  // No radio in the emulator: keep the app "offline" on purpose (feeds, OTA,
  // MQTT and ESP-NOW follow their WiFi-down paths; the smoke test asserts one).
  Serial.println("[SYS] QEMU: wifi-task skipped (no radio in emulator)");
#else
  net_wifi::start();
#endif
  mqtt_mgr::applyConfig(); // arms retry pump (WiFi-gated); status -> UI via 1 Hz sync
  mqttSyncUi();

  // prime loop() cadence: first relay probe ~15 s after boot
  s_t_1s = millis();
  s_t_30s = millis();
  s_t_60s = millis() - 45000UL;

  Serial.println("[SYS] setup complete");
}

void loop() {
#if CATFISH_QEMU
  qemuSmokePump();
#endif
  const uint32_t now = millis();

  // OTA updater: begin lazily once WiFi/mDNS is up, then serve every pass.
  beginOtaOnce();
  if (s_ota_ready) ArduinoOTA.handle();

  // ESP-NOW: start listening once the STA has a channel (v1.8.0)
  if (net_espnow::state() == net_espnow::State::WaitingWifi &&
      net_wifi::wifi_is_connected()) {
    net_espnow::beginOnce();
  }

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

    // v1.6.0: once NTP has actually synced (not the RTC boot restore), write
    // the wall clock back into the PCF85063 so the CR927 cell stays current.
    // v1.8.4: also re-write hourly — corrects chip drift, refreshes the cell
    // backup on long uptimes, and recovers a first write that lost an I2C
    // transaction to a concurrent touch poll.
    if (s_rtc_present && !s_rtc_ntp_synced &&
        sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      if (pcf85063::writeUtc(time(nullptr))) {
        s_rtc_ntp_synced = true;
        s_rtc_last_write_ms = now;
        if (s_rtc_status != "battery OK") {
          s_rtc_status = "battery OK";
          lvgl_port_lock(-1);
          ui_set_rtc(s_rtc_status);
          lvgl_port_unlock();
        }
        Serial.println("[SYS] RTC updated from NTP");
      }
    } else if (s_rtc_present && s_rtc_ntp_synced && s_rtc_last_write_ms != 0 &&
               time(nullptr) > CATFISH_RTC_MIN_EPOCH &&
               (now - s_rtc_last_write_ms) >= 3600000UL) {
      if (pcf85063::writeUtc(time(nullptr))) {
        s_rtc_last_write_ms = now;
        Serial.println("[SYS] RTC drift-correction rewrite");
      }
    }

    // scheduled feeding (targets restricted to the configured tanks)
    const FeedTime due = feed_scheduler::tick();
    if (due.enabled) {
      const uint8_t mask = effectiveTankMask(due, tankCountClamped());
      // v1.9.0 (E4): one-shot skip of this occurrence
      if (feed_state::consumeSkip(due.hour, due.minute)) {
        feed_state::markFired(due.hour, due.minute, (uint32_t)time(nullptr));
        feed_state::recordHistory(mask, 0, due.grams, false,
                                  feed_state::HIST_SKIPPED);
        uiToast(String("Skipped ") + fmtHhmm(due.hour, due.minute));
        Serial.printf("[FEED] skipped scheduled feed %02u:%02u (skip-next)\n",
                      (unsigned)due.hour, (unsigned)due.minute);
      } else {
        // fired-map marker BEFORE firing (crash-safe: failures are the
        // journal's job; catch-up must not double-fire this slot)
        feed_state::markFired(due.hour, due.minute, (uint32_t)time(nullptr));
        Serial.printf("[FEED] scheduled feed %02u:%02u (%u g, tanks 0x%X)\n",
                      (unsigned)due.hour, (unsigned)due.minute,
                      (unsigned)due.grams, (unsigned)mask);
        uiToast("Feeding time!");
        requestFeed(due.grams, false, due.hour, due.minute, false, mask,
                    due.gramTank, feed_state::SRC_SCHED);
      }
    }

    // v1.9.0 (B1): persistent retry journal (survives power cycles)
    feed_state::RetryDue rd;
    if (feed_state::journalDue((uint32_t)time(nullptr), rd)) {
      Serial.printf("[FEED] retrying scheduled feed %02u:%02u (%u g, tanks 0x%X)\n",
                    (unsigned)rd.hour, (unsigned)rd.minute,
                    (unsigned)rd.grams, (unsigned)rd.tankMask);
      requestFeed(rd.grams, false, rd.hour, rd.minute, true, rd.tankMask,
                  nullptr, feed_state::SRC_SCHED);
    }

    // v1.9.0 (B3): fire armed pulse-verify probes (mid-pulse)
    for (uint8_t t = 1; t <= relay_client::MAX_TANKS; t++) {
      FeedVerify &v = s_verify[t - 1];
      if (v.armed && (int32_t)(now - v.dueMs) >= 0) {
        if (net_worker::enqueueProbe(false, t, true)) {
          v.armed = false;
          v.pending = true;
        } // enqueue full -> stays armed, retried next tick
      }
    }

    // v1.9.0 (A4): catch-up scan once the wall clock first becomes valid
    // (RTC boot restore OR first NTP sync — whichever comes first)
    if (!s_catchup_done && feed_scheduler::timeValid()) {
      s_catchup_done = true;
      runCatchupScan();
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
    radiosUiSync();
    wifiMgrUiSync(); // v1.10.0: WiFi manager scan/connect lifecycle

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

  // ---------------- 10 s block: SD hot-plug recheck (v1.8.4) ----------------
  // While no card is mounted, retry one mount speed per pass — rescues cards
  // inserted after boot and marginal cards that need a slower SPI clock. A
  // cardless slot costs ≤ ~0.5 s per recheck (see sd_log::remount()).
  if (sd_log::state() == sd_log::State::NoCard &&
      (now - s_t_sd_recheck) >= 10000UL) {
    s_t_sd_recheck = now;
    if (sd_log::remount()) {
      Serial.println("[SD] card mounted on recheck");
      uiToast("SD card detected");
      lvgl_port_lock(-1);
      ui_set_sd(sd_log::summary());
      lvgl_port_unlock();
    }
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
