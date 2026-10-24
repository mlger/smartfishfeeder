/**
 * @file ui.h
 * @brief LVGL v8.4 UI for the Catfish Feeder display unit (800x480).
 *
 * Threading contract:
 *  - Every ui_* function must be called while the caller holds
 *    lvgl_port_lock() (main loop does; LVGL callbacks run inside the LVGL
 *    task which already owns the mutex).
 *  - Event callbacks NEVER do blocking network I/O, flash writes, or
 *    scheduler mutations. They only enqueue a UiEvent (FreeRTOS queue),
 *    which loop() consumes.
 */
#ifndef CATFISH_UI_H
#define CATFISH_UI_H

#include <Arduino.h>
#include <stdint.h>

// Shared protocol constants: CATFISH_APP_VERSION below and the MQTT field
// caps used by UiEvent come from here (dependency-free header).
#include <catfish_protocol.h>

// Single source of truth is firmware/common/catfish_protocol.h
// (CATFISH_APP_VERSION, shared with the relay unit); this fallback keeps IDE
// tooling happy and avoids macro redefinition.
#ifndef CATFISH_APP_VERSION
#define CATFISH_APP_VERSION "1.0.0"
#endif

// ---- Events: LVGL callbacks -> loop() (payload-bearing, race-free) ----
// The queue is also written from the MQTT manager's async task (incoming
// Home Assistant commands), which is why ui_event_send() is public.
enum UiEventType : uint8_t {
  UI_EVT_FEED = 0,      // FEED NOW pressed      (a=seconds, flag=manual)
  UI_EVT_RELAY_TEST,    // "Test connection" pressed
  UI_EVT_REBOOT,        // "Reboot" pressed
  UI_EVT_SET_PULSE,     // default pulse changed (a=seconds)
  UI_EVT_SET_TOKEN,     // pairing token saved   (token=new, flag=also rotate relay)
  UI_EVT_SET_BL,        // backlight switch flipped (flag=on; loop persists)
  UI_EVT_SCHED_ADD,     // editor Save           (a=h, b=m, c=s)
  UI_EVT_SCHED_REMOVE,  // trash pressed         (a=h, b=m, c=s — identity)
  UI_EVT_SCHED_PERSIST, // enable switch flipped (a=index, flag=enabled — loop applies + flushes)
  UI_EVT_MQTT_CFG,      // MQTT settings saved   (mqtt* fields valid — loop persists + applies)
};

struct UiEvent {
  UiEventType type;
  uint8_t a = 0;
  uint8_t b = 0;
  uint8_t c = 0;
  bool flag = false;
  char token[24] = {0}; // == CATFISH_TOKEN_LEN (23 chars + NUL)
  // MQTT settings (UI_EVT_MQTT_CFG); sizes == prefs/catfish_protocol caps
  bool mqttEnable = false;
  uint16_t mqttPort = 0;
  char mqttHost[CATFISH_MQTT_HOST_LEN] = {0};
  char mqttUser[CATFISH_MQTT_USER_LEN] = {0};
  char mqttPass[CATFISH_MQTT_PASS_LEN] = {0};
  char mqttPrefix[CATFISH_MQTT_PREFIX_LEN] = {0};
};

namespace ui {

/** Build all three screens + overlays. Caller must hold lvgl_port_lock(). */
void ui_init();

/** 1 Hz housekeeping: clock, next-feed countdown, relay dot pulse. Caller holds lock. */
void ui_tick();

/** Non-blocking event drain for loop(). @return false when the queue is empty. */
bool ui_event_receive(UiEvent &out);

/**
 * Enqueue an event from OUTSIDE the LVGL task (e.g. mqtt_mgr's AsyncTCP
 * callback thread). Same queue the LVGL callbacks use; never blocks.
 */
bool ui_event_send(const UiEvent &ev);

/** Settings > MQTT card status line. state: 0 muted, 1 busy, 2 good, 3 bad. */
void ui_set_mqtt_status(const String &text, uint8_t state);

/** Settings > MQTT card broker summary, e.g. "Broker: 192.168.1.10:1883". */
void ui_set_mqtt_broker(const String &text);

/** Reflect externally changed default pulse (Home Assistant) in the spinbox. */
void ui_set_pulse(uint8_t seconds);

/** WiFi state for the status-bar symbol and Settings > Network card. */
void ui_set_wifi(bool connected, const String &ssid, const String &ip, int rssi);

/** Relay unit state for Home card and Settings card (feeding shows on Home). */
void ui_set_relay(bool online, const String &ip, bool feeding = false);

/** Free-text next-feed line (also clears the structured countdown target). */
void ui_set_next_feed(const String &text);

/**
 * Structured next-feed target so ui_tick() can render a live countdown
 * ("07:30  •  in 2 h 15 m" / "Tomorrow 07:30  •  ..."). valid=false -> "—".
 */
void ui_set_next_feed_time(int hour, int minute, bool tomorrow, bool valid);

/** "Last fed" card text, e.g. "Today 07:30". */
void ui_set_last_feed(const String &text);

/** Transient toast message (auto-hides after ~2.2 s). */
void ui_toast(const String &msg);

/** Register the backlight on/off handler (no PWM on this board) + initial state. */
void ui_set_backlight_cb(void (*cb)(bool on), bool initial_on);

/** Request a manual feed (safe to call from LVGL callbacks). */
void ui_request_feed(uint8_t seconds);

/** Disable/re-enable FEED NOW while a feed request is in flight. */
void ui_set_feed_busy(bool busy);

/** Disable/re-enable "Test connection" while a relay probe is in flight. */
void ui_set_probe_busy(bool busy);

/** Rebuild the schedule list from feed_scheduler (caller holds the lock). */
void ui_schedule_refresh();

/** Close the schedule editor overlay (e.g. after a successful add). */
void ui_editor_close();

} // namespace ui

#endif // CATFISH_UI_H
