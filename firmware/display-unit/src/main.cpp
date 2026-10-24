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

#include "lvgl_v8_port.h"
#include "esp_display_panel.hpp"

#include "feed_scheduler.h"
#include "net_wifi.h"
#include "net_worker.h"
#include "prefs.h"
#include "relay_client.h"
#include "ui/ui.h"

using namespace esp_panel::board;
using namespace ui; // UI entry points (ui_init/ui_tick/ui_set_*/UiEvent)

static Board *s_board = nullptr;

// loop() cadence timestamps
static uint32_t s_t_1s = 0;
static uint32_t s_t_30s = 0;
static uint32_t s_t_60s = 0;

// Scheduled-feed retry (one retry, ~30 s later)
struct SchedRetry {
  bool pending = false;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t seconds = 0;
  uint32_t dueMs = 0;
};
static SchedRetry s_retry;

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

static void uiToast(const String &msg) {
  lvgl_port_lock(-1);
  ui_toast(msg);
  lvgl_port_unlock();
}

/** Route a feed request to the worker; manual feeds get immediate feedback. */
static void requestFeed(uint8_t seconds, bool manual, uint8_t hour,
                        uint8_t minute, bool retry) {
  if (!net_wifi::wifi_is_connected()) {
    uiToast("WiFi not connected");
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
      if (!net_worker::enqueueProbe(true))
        uiToast("Too busy, try again");
      break;

    case UI_EVT_REBOOT:
      Serial.println("[SYS] reboot requested");
      feed_scheduler::flushIfDirty();
      delay(300);
      ESP.restart();
      break;

    case UI_EVT_SET_PULSE:
      prefs::setDefaultPulse(ev.a);
      Serial.printf("[FEED] default pulse set to %u s\n", (unsigned)ev.a);
      break;

    case UI_EVT_SET_TOKEN:
      prefs::setRelayToken(String(ev.token));
      Serial.println("[NET] pairing token updated");
      uiToast("Pairing token updated");
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
      feed_scheduler::flushIfDirty();
      refreshNextFeedUi(); // enable/disable changes the next occurrence
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Worker result handling
// ---------------------------------------------------------------------------
static void handleWorkerResult(const net_worker::Result &r) {
  if (r.kind == net_worker::Result::RES_FEED) {
    lvgl_port_lock(-1);
    ui_set_feed_busy(false);
    lvgl_port_unlock();

    if (r.ok) {
      s_retry.pending = false;
      const uint32_t epoch = (uint32_t)time(nullptr);
      prefs::setLastFedEpoch(epoch);
      const String when = r.manual
                              ? formatLastFed(epoch)
                              : (String("Today ") + fmtHhmm(r.hour, r.minute));
      lvgl_port_lock(-1);
      ui_set_last_feed(when);
      ui_toast(r.manual ? (String("Fed OK (") + r.seconds + " s)")
                        : (String("Fed ") + fmtHhmm(r.hour, r.minute)));
      lvgl_port_unlock();
      Serial.printf("[FEED] feed OK via %s (%u s)\n", r.ip, (unsigned)r.seconds);
      refreshNextFeedUi();
    } else if (r.manual) {
      lvgl_port_lock(-1);
      ui_toast(String("Feed failed: ") + r.error);
      lvgl_port_unlock();
      Serial.printf("[FEED] manual feed FAILED: %s\n", r.error);
    } else {
      // scheduled feed
      if (r.retry || s_retry.pending) {
        s_retry.pending = false;
        uiToast(String("Feed FAILED: ") + r.error);
        Serial.printf("[FEED] scheduled feed FAILED (no more retries): %s\n",
                      r.error);
      } else {
        s_retry.pending = true;
        s_retry.hour = r.hour;
        s_retry.minute = r.minute;
        s_retry.seconds = r.seconds;
        s_retry.dueMs = millis() + 30000UL;
        uiToast("Feed failed - retrying in 30 s");
        Serial.printf("[FEED] scheduled feed failed (%s), retry armed\n",
                      r.error);
      }
    }
    return;
  }

  // RES_PROBE
  lvgl_port_lock(-1);
  ui_set_relay(r.online, r.ip, r.feeding);
  if (r.report) {
    if (!net_wifi::wifi_is_connected())
      ui_toast("WiFi not connected");
    else if (r.online)
      ui_toast(String("Relay OK  v") + r.version);
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

  // --- background tasks ---
  if (!net_worker::begin()) {
    Serial.println("[SYS] ERROR: net worker unavailable; feeds will fail");
  }
  net_wifi::start();

  // prime loop() cadence: first relay probe ~15 s after boot
  s_t_1s = millis();
  s_t_30s = millis();
  s_t_60s = millis() - 45000UL;

  Serial.println("[SYS] setup complete");
}

void loop() {
  const uint32_t now = millis();

  // ---- drain UI events + worker results (every pass, ~50 ms latency) ----
  UiEvent ev;
  while (ui_event_receive(ev)) handleUiEvent(ev);

  net_worker::Result res;
  while (net_worker::receive(res)) handleWorkerResult(res);

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

    // armed retry for a failed scheduled feed
    if (s_retry.pending && (int32_t)(now - s_retry.dueMs) >= 0) {
      s_retry.pending = false;
      Serial.printf("[FEED] retrying scheduled feed %02u:%02u (%u s)\n",
                    (unsigned)s_retry.hour, (unsigned)s_retry.minute,
                    (unsigned)s_retry.seconds);
      requestFeed(s_retry.seconds, false, s_retry.hour, s_retry.minute, true);
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

  // ---------------- 60 s block: relay probe (via worker, only if WiFi up) ----
  if ((now - s_t_60s) >= 60000) {
    s_t_60s = now;
    if (net_wifi::wifi_is_connected()) {
      net_worker::enqueueProbe(false);
    }
  }

  delay(50);
}
