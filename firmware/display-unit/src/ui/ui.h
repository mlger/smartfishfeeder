/**
 * @file ui.h
 * @brief LVGL v8.4 UI for the Catfish Feeder display unit (800x480).
 *
 * Threading contract:
 *  - Every ui_* function must be called while the caller holds
 *    lvgl_port_lock() (main loop does; LVGL callbacks run inside the LVGL
 *    task which already owns the mutex).
 *  - Event callbacks NEVER do blocking network I/O; they only set the
 *    volatile request flags below, which loop() consumes.
 */
#ifndef CATFISH_UI_H
#define CATFISH_UI_H

#include <Arduino.h>
#include <stdint.h>

#define CATFISH_APP_VERSION "0.1.0"

// ---- Cross-thread request flags (written by LVGL callbacks, consumed by loop()) ----
extern volatile bool   g_feed_request;      // manual FEED NOW pressed
extern volatile uint8_t g_feed_seconds;     // requested pulse seconds
extern volatile bool   g_relay_test_request; // Settings "Test connection" pressed
extern volatile bool   g_reboot_request;     // Settings "Reboot" pressed
extern volatile bool   g_schedule_dirty;     // schedule CRUD happened; loop() refreshes next-feed

namespace ui {

/** Build all three screens + overlays. Caller must hold lvgl_port_lock(). */
void ui_init();

/** 1 Hz housekeeping: clock, next-feed countdown, relay dot pulse. Caller holds lock. */
void ui_tick();

/** WiFi state for the status-bar symbol and Settings > Network card. */
void ui_set_wifi(bool connected, String ssid, String ip, int rssi);

/** Relay unit state for Home card and Settings card. */
void ui_set_relay(bool online, String ip);

/** Free-text next-feed line (also clears the structured countdown target). */
void ui_set_next_feed(String text);

/**
 * Structured next-feed target so ui_tick() can render a live countdown
 * ("07:30  •  in 2 h 15 m" / "Tomorrow 07:30  •  ..."). valid=false -> "—".
 */
void ui_set_next_feed_time(int hour, int minute, bool tomorrow, bool valid);

/** "Last fed" card text, e.g. "Today 07:30". */
void ui_set_last_feed(String text);

/** Transient toast message (auto-hides after ~2.2 s). */
void ui_toast(String msg);

/** Register the backlight on/off handler (no PWM on this board) + initial state. */
void ui_set_backlight_cb(void (*cb)(bool on), bool initial_on);

/** Request a manual feed (safe to call from LVGL callbacks). */
void ui_request_feed(uint8_t seconds);

} // namespace ui

#endif // CATFISH_UI_H
