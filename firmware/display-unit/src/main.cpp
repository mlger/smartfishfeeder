/**
 * @file main.cpp
 * @brief Catfish Feeder display unit — ESP32-S3-Touch-LCD-4.3B (800x480 RGB).
 *
 * Architecture:
 *  - LVGL runs in its own FreeRTOS task (lvgl_v8_port), touch via GT911.
 *  - "wifi-task" (core 0) owns WiFiManager/mDNS/NTP; main loop stays free.
 *  - loop(): 1 Hz scheduler tick + manual-feed handling + ui_tick(),
 *    30 s WiFi UI refresh, 60 s relay probe. Blocking only for feed POSTs.
 *
 * Serial log tags: [FEED] [NET] [UI] [SYS].
 */
#include <Arduino.h>
#include <time.h>

#include "lvgl_v8_port.h"
#include "esp_display_panel.hpp"

#include "feed_scheduler.h"
#include "net_wifi.h"
#include "prefs.h"
#include "relay_client.h"
#include "ui/ui.h"

using namespace esp_panel::board;

static Board *s_board = nullptr;

// loop() cadence timestamps
static uint32_t s_t_1s = 0;
static uint32_t s_t_30s = 0;
static uint32_t s_t_60s = 0;

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

/** Recompute next feeding from the scheduler and push it to the UI. */
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
}

/**
 * Execute a feed command (blocking HTTP POST, up to ~6 s).
 * Manual feeds: relay token + default pulse; scheduled: entry time shown.
 */
static void doFeed(uint8_t seconds, bool manual, uint8_t feedHour = 0,
                   uint8_t feedMinute = 0) {
  if (!net_wifi::wifi_is_connected()) {
    lvgl_port_lock(-1);
    ui_toast("WiFi not connected");
    lvgl_port_unlock();
    Serial.println("[FEED] aborted: WiFi down");
    return;
  }

  lvgl_port_lock(-1);
  ui_toast(manual ? "Feeding fish..." : "Feeding time!");
  lvgl_port_unlock();

  const relay_client::Result r = relay_client::feed(seconds);
  bool ok = r.ok;
  String when;

  if (ok) {
    const uint32_t epoch = (uint32_t)time(nullptr);
    prefs::setLastFedEpoch(epoch);
    when = manual ? formatLastFed(epoch)
                  : (String("Today ") + fmtHhmm(feedHour, feedMinute));
    Serial.printf("[FEED] feed OK via %s (%u s)\n", r.ip.c_str(),
                  (unsigned)seconds);
  } else {
    Serial.printf("[FEED] feed FAILED: %s\n", r.error.c_str());
  }

  lvgl_port_lock(-1);
  if (ok) {
    ui_set_last_feed(when);
    ui_toast(manual ? (String("Fed OK (") + seconds + " s)")
                    : (String("Fed ") + fmtHhmm(feedHour, feedMinute)));
  } else {
    ui_toast(String("Feed failed: ") + r.error);
  }
  lvgl_port_unlock();

  if (ok) refreshNextFeedUi();
}

/** Probe relay (fetchStatus, discovering first when needed) and update UI. */
static void probeRelay(bool report) {
  bool online = false;
  String version;

  if (net_wifi::wifi_is_connected()) {
    if (!relay_client::hasIp()) {
      const relay_client::Result d = relay_client::discover();
      online = d.ok;
    }
    if (relay_client::hasIp() || online) {
      const relay_client::Status st = relay_client::fetchStatus();
      online = st.ok;
      if (st.ok) version = st.version;
    }
  }

  // Show the cached IP either way: "Online • ip" or "Offline • ip".
  // With no IP at all the UI falls back to "Searching...".
  const String shown_ip = relay_client::hasIp() ? relay_client::ip() : String("");

  lvgl_port_lock(-1);
  ui_set_relay(online, shown_ip);
  if (report) {
    if (!net_wifi::wifi_is_connected())
      ui_toast("WiFi not connected");
    else if (online)
      ui_toast(String("Relay OK  v") + version);
    else
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

  // --- build UI under lock ---
  lvgl_port_lock(-1);
  ui_init();
  ui_set_backlight_cb(appSetBacklight, true);
  ui_set_relay(false, prefs::getRelayIp());
  ui_set_last_feed(formatLastFed(prefs::getLastFedEpoch()));
  lvgl_port_unlock();
  refreshNextFeedUi();

  // --- wifi (own task, LVGL stays responsive) ---
  net_wifi::start();

  // prime loop() cadence: first relay probe ~15 s after boot
  s_t_1s = millis();
  s_t_30s = millis();
  s_t_60s = millis() - 45000UL;

  Serial.println("[SYS] setup complete");
}

void loop() {
  const uint32_t now = millis();

  // ---------------- 1 Hz block ----------------
  if ((now - s_t_1s) >= 1000) {
    s_t_1s = now;

    // scheduled feeding
    const FeedTime due = feed_scheduler::tick();
    if (due.enabled) {
      Serial.printf("[FEED] scheduled feed %02u:%02u (%u s)\n",
                    (unsigned)due.hour, (unsigned)due.minute,
                    (unsigned)due.seconds);
      doFeed(due.seconds, false, due.hour, due.minute);
    }

    // manual FEED NOW from UI
    if (g_feed_request) {
      const uint8_t secs = g_feed_seconds;
      g_feed_request = false;
      Serial.printf("[FEED] manual feed %u s\n", (unsigned)secs);
      doFeed(secs, true);
    }

    // relay "Test connection" from Settings
    if (g_relay_test_request) {
      g_relay_test_request = false;
      Serial.println("[NET] relay test requested");
      probeRelay(true);
    }

    // reboot from Settings
    if (g_reboot_request) {
      g_reboot_request = false;
      Serial.println("[SYS] reboot requested");
      delay(300);
      ESP.restart();
    }

    // schedule changed on UI -> recompute next feeding
    if (g_schedule_dirty) {
      g_schedule_dirty = false;
      refreshNextFeedUi();
    }

    // UI housekeeping (clock, countdown, relay dot)
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

  // ---------------- 60 s block: relay probe (only if WiFi up) ----------------
  if ((now - s_t_60s) >= 60000) {
    s_t_60s = now;
    if (net_wifi::wifi_is_connected()) {
      probeRelay(false);
    }
  }

  delay(50);
}
