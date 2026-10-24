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
  UI_EVT_FEED = 0,      // FEED NOW pressed      (a=seconds, flag=manual, tankMask targets)
  UI_EVT_RELAY_TEST,    // "Test connection" pressed
  UI_EVT_REBOOT,        // "Reboot" pressed
  UI_EVT_SET_PULSE,     // default pulse changed (a=seconds)
  UI_EVT_SET_TOKEN,     // pairing token saved   (token=new, flag=also rotate relay)
  UI_EVT_SET_BL,        // backlight switch flipped (flag=on; loop persists)
  UI_EVT_SET_FMT12,     // v1.6.0: 12/24-hour clock switch (flag=true = 12-hour)
  UI_EVT_SCHED_ADD,     // editor Save           (a=h, b=m, c=s, tankMask=target tanks)
  UI_EVT_SCHED_REMOVE,  // trash pressed         (a=h, b=m, c=s — identity)
  UI_EVT_SCHED_PERSIST, // enable switch flipped (a=index, flag=enabled — loop applies + flushes)
  UI_EVT_MQTT_CFG,      // MQTT settings saved   (mqtt* fields valid — loop persists + applies)
  UI_EVT_MECH_CFG,      // P4: mechanism saved   (mech* fields valid — loop POSTs /config)
  UI_EVT_SELECT_TANK,   // P5: tank chip tapped  (a=tank index 0..3, 0xFE = All)
  UI_EVT_TANK_COUNT,    // P5: tank count changed (a=new count 1..4)
  UI_EVT_TANK_CFG,      // P5: tank saved (a=tank id 1-based, tankName/tankToken valid)
  UI_EVT_SCAN_TANKS,    // P5: "Scan for relays" pressed
};

struct UiEvent {
  UiEventType type;
  uint8_t a = 0;
  uint8_t b = 0;
  uint8_t c = 0;
  bool flag = false;
  char token[24] = {0}; // == CATFISH_TOKEN_LEN (23 chars + NUL)
  uint8_t tankMask = 0; // FEED/SCHED_ADD: target tanks (bit i = tank i+1); 0 = tank 1
  // MQTT settings (UI_EVT_MQTT_CFG); sizes == prefs/catfish_protocol caps
  bool mqttEnable = false;
  uint16_t mqttPort = 0;
  char mqttHost[CATFISH_MQTT_HOST_LEN] = {0};
  char mqttUser[CATFISH_MQTT_USER_LEN] = {0};
  char mqttPass[CATFISH_MQTT_PASS_LEN] = {0};
  char mqttPrefix[CATFISH_MQTT_PREFIX_LEN] = {0};
  // Feeding mechanism (UI_EVT_MECH_CFG, P4); pre-clamped by the UI
  uint8_t mechId = 0;   // CATFISH_MECH_*
  uint8_t mechRest = 0; // servo rest angle
  uint8_t mechFeed = 0; // servo feed angle
  uint16_t mechHz = 0;  // stepper step rate
  uint8_t mechDir = 0;  // stepper DIR level during feed
  // Tank editor (UI_EVT_TANK_CFG, P5)
  char tankName[CATFISH_TANK_NAME_LEN] = {0}; // == 16 (15 chars + NUL)
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

/**
 * P4: Settings > Relay card "Mechanism" line + overlay prefill cache.
 * mechName "" = pre-P4 relay firmware (shown as "not reported").
 * Angles/rate -1 = unknown; used to prefill the mechanism editor.
 */
void ui_set_mech(const String &mechName, int restDeg, int feedDeg, int stepHz);

// ---- multi-tank (Phase 5) ---------------------------------------------------
/**
 * Rebuild the tank-dependent UI from source data: Home tank chips
 * (incl. "All" chip when count >= 2), Settings > TANKS rows and the
 * schedule editor's tank checkboxes. Caller holds the lock.
 * @param count    configured tank count 1..CATFISH_MAX_TANKS
 * @param names    per-tank display names (indexes 0..3)
 * @param selected currently selected tank (0-based index, or 0xFE = All)
 */
void ui_tanks_rebuild(uint8_t count, const String names[CATFISH_MAX_TANKS],
                      uint8_t selected);

/**
 * Refresh live state on the already-built tank widgets (chip + row colors,
 * per-tank IP labels). Cheap; safe to call every probe. Caller holds lock.
 */
void ui_tanks_state(const bool online[CATFISH_MAX_TANKS],
                    const bool feeding[CATFISH_MAX_TANKS],
                    const String ips[CATFISH_MAX_TANKS]);

/** Disable/re-enable "Scan for relays" while a scan is in flight. */
void ui_set_scan_busy(bool busy);

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

/**
 * v1.6.0: switch the clock presentation (true = 12-hour "7:30 AM", false =
 * 24-hour). Rebuilds the hour roller options + re-renders schedule labels on
 * the next refresh; cached labels repaint on the next ui_tick().
 */
void ui_set_hour12(bool h12);

/** v1.6.0: Settings > System RTC status line (battery/clock state). */
void ui_set_rtc(const String &text);

/** v1.7.0: Settings > STORAGE card SD-card summary ("no card", "1.9 GB free"). */
void ui_set_sd(const String &text);

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
