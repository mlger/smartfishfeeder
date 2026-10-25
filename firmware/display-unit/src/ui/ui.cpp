/**
 * @file ui.cpp
 * @brief LVGL v8.4 UI — Catfish Feeder display unit, 800x480, aquarium theme.
 *
 * Theme: deep teal/charcoal background (#10201F), warm amber accent (#F2A33C),
 * white text, 12px-radius cards, 16-24px padding, touch targets >= 64 px.
 *
 * Threading: all ui_* entry points are called under lvgl_port_lock() by the
 * main loop; LVGL event callbacks run inside the LVGL task (mutex owned).
 * Callbacks never block: they enqueue a UiEvent (FreeRTOS queue) consumed by
 * loop(). No flash writes, no scheduler mutations, no network I/O here.
 */
#include "ui.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <catfish_protocol.h>
#include <catfish_mech.h>
#include <catfish_time.h>
#include "../feed_scheduler.h"
#include "../prefs.h"

// ---------------------------------------------------------------------------
// Event queue: LVGL callbacks -> loop()
// ---------------------------------------------------------------------------
static QueueHandle_t s_evt_queue = nullptr;

static bool uiEventSend(const UiEvent &ev) {
  if (!s_evt_queue) return false;
  if (xQueueSend(s_evt_queue, &ev, 0) != pdTRUE) {
    Serial.println("[UI] WARN: event queue full, event dropped");
    return false;
  }
  return true;
}

namespace ui {

namespace {

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
constexpr uint32_t C_BG        = 0x10201F; // deep teal/charcoal background
constexpr uint32_t C_BG_DARK   = 0x0B1615; // bars
constexpr uint32_t C_CARD      = 0x1A2E2C; // card surface
constexpr uint32_t C_CARD_HI   = 0x243F3B; // pressed card
constexpr uint32_t C_AMBER     = 0xF2A33C; // warm amber accent
constexpr uint32_t C_AMBER_HI  = 0xD98F2B; // amber pressed
constexpr uint32_t C_TEXT      = 0xF2F7F6; // near-white
constexpr uint32_t C_MUTED     = 0x8FA9A4; // muted gray-teal
constexpr uint32_t C_GREEN     = 0x46D07E; // online
constexpr uint32_t C_RED       = 0xD96459; // destructive/offline
constexpr uint32_t C_INK       = 0x121E1C; // text on amber

// ---------------------------------------------------------------------------
// Widget handles
// ---------------------------------------------------------------------------
lv_obj_t *s_scr[3] = {nullptr, nullptr, nullptr};          // home, schedule, settings
lv_obj_t *s_clock_lbl[3] = {nullptr, nullptr, nullptr};
lv_obj_t *s_wifi_sym[3] = {nullptr, nullptr, nullptr};
lv_obj_t *s_nav_btn[3][3] = {};

// Home
lv_obj_t *s_feed_btn = nullptr;
lv_obj_t *s_next_value = nullptr;
lv_obj_t *s_next_sub = nullptr;
lv_obj_t *s_last_value = nullptr;
lv_obj_t *s_relay_dot = nullptr;
lv_obj_t *s_relay_txt = nullptr;
lv_obj_t *s_tank_chips = nullptr; // P5: tank selection chip row (hidden at count 1)

// Schedule
lv_obj_t *s_sched_list = nullptr;
lv_obj_t *s_editor = nullptr;
lv_obj_t *s_r_hour = nullptr;
lv_obj_t *s_r_min = nullptr;
lv_obj_t *s_sb_sec = nullptr;
lv_obj_t *s_ed_tank_row = nullptr;  // P5: tank checkbox row inside the editor
lv_obj_t *s_ed_tank_cb[CATFISH_MAX_TANKS] = {nullptr, nullptr, nullptr, nullptr};

// Settings
lv_obj_t *s_sb_pulse = nullptr;
lv_obj_t *s_sw_bl = nullptr;
lv_obj_t *s_sw_h12 = nullptr; // v1.6.0: 12-hour clock switch
lv_obj_t *s_lbl_rtc = nullptr; // v1.6.0: Settings > System RTC status line
lv_obj_t *s_lbl_sd = nullptr;  // v1.7.0: Settings > STORAGE card summary
lv_obj_t *s_lbl_espnow = nullptr; // v1.8.0: RADIOS card ESP-NOW line
lv_obj_t *s_lbl_rf24 = nullptr;   // v1.8.0: RADIOS card nRF24L01 line
lv_obj_t *s_lbl_ssid = nullptr;
lv_obj_t *s_lbl_ip = nullptr;
lv_obj_t *s_lbl_rssi = nullptr;
lv_obj_t *s_lbl_relay_ip = nullptr;
lv_obj_t *s_lbl_mech = nullptr; // P4: "Mechanism: ..." line on the Relay card
lv_obj_t *s_probe_btn = nullptr;
lv_obj_t *s_tank_rows = nullptr;      // P5: TANKS card rows container
lv_obj_t *s_tank_sb_count = nullptr;  // P5: tank count spinbox
lv_obj_t *s_scan_btn = nullptr;       // P5: "Scan for relays"
lv_obj_t *s_tank_row_name[CATFISH_MAX_TANKS] = {nullptr, nullptr, nullptr, nullptr};
lv_obj_t *s_tank_row_ip[CATFISH_MAX_TANKS] = {nullptr, nullptr, nullptr, nullptr};
lv_obj_t *s_tank_row_dot[CATFISH_MAX_TANKS] = {nullptr, nullptr, nullptr, nullptr};

// Settings > MQTT card + editor overlay (lv_layer_top)
lv_obj_t *s_mq_status = nullptr;  // status line (color-coded)
lv_obj_t *s_mq_broker = nullptr;  // "Broker: host:port • prefix/..."
lv_obj_t *s_mq_overlay = nullptr;
lv_obj_t *s_mq_sw_en = nullptr;   // "Enabled" switch inside the editor
lv_obj_t *s_mq_ta_host = nullptr;
lv_obj_t *s_mq_sb_port = nullptr;
lv_obj_t *s_mq_ta_prefix = nullptr;
lv_obj_t *s_mq_ta_user = nullptr;
lv_obj_t *s_mq_ta_pass = nullptr;
lv_obj_t *s_mq_kb = nullptr;

// Token editor overlay (lv_layer_top)
lv_obj_t *s_tok_overlay = nullptr;
lv_obj_t *s_tok_ta = nullptr;
lv_obj_t *s_tok_kb = nullptr;
lv_obj_t *s_tok_rotate = nullptr; // "also update relay unit" checkbox

// Mechanism editor overlay (P4, lv_layer_top; roller + spinboxes, no keyboard)
lv_obj_t *s_mech_overlay = nullptr;
lv_obj_t *s_mech_roller = nullptr;
lv_obj_t *s_mech_row_servo = nullptr; // rest/feed angle row (visible when servo)
lv_obj_t *s_mech_row_step = nullptr;  // stepHz + direction row (visible when stepper)
lv_obj_t *s_mech_sb_rest = nullptr;
lv_obj_t *s_mech_sb_feed = nullptr;
lv_obj_t *s_mech_sb_hz = nullptr;
lv_obj_t *s_mech_sw_dir = nullptr;

// Tank editor overlay (P5, lv_layer_top: name + token override + keyboard)
lv_obj_t *s_tank_overlay = nullptr;
lv_obj_t *s_tank_ta_name = nullptr;
lv_obj_t *s_tank_ta_tok = nullptr;
lv_obj_t *s_tank_sb_gps = nullptr;  // v1.9.0 (A3): food rate g/s (0 = off)
lv_obj_t *s_tank_sb_hcap = nullptr; // v1.9.0 (D1): hopper size in seconds
lv_obj_t *s_tank_kb = nullptr;
uint8_t s_tank_edit_id = 1; // tank being edited (1-based)

// Toast
lv_obj_t *s_toast_box = nullptr;
lv_obj_t *s_toast_lbl = nullptr;
lv_timer_t *s_toast_tmr = nullptr;

// State
void (*s_bl_cb)(bool) = nullptr;
bool s_bl_on = true;   // user preference (persisted via UI_EVT_SET_BL)
bool s_dimmed = false; // transient: screen auto-dimmed after idle timeout
bool s_hour12 = false; // v1.6.0: clock presentation (persisted via UI_EVT_SET_FMT12)
bool s_relay_online = false;
bool s_relay_feeding = false;
bool s_dot_blink = false;
bool s_next_valid = false;
bool s_next_tomorrow = false;
int s_next_hour = 0;
int s_next_minute = 0;

char s_relay_ip_cache[16] = "";
char s_hour_opts[168] = ""; // 24h "00".."23" or 12h "12 AM".."11 PM" (v1.6.0)
char s_min_opts[216] = "";
char s_clock_cache[12] = ""; // "07:30" or "11:59 PM"
char s_val_cache[24] = "";
char s_sub_cache[28] = "";

// P4 mechanism cache (last /status probe; -1 = unknown) — overlays prefill here
bool s_mech_reported = false; // relay firmware reports "mechanism"
uint8_t s_mech_id = CATFISH_MECH_DEFAULT;
int s_mech_rest = -1;
int s_mech_feed = -1;
int s_mech_hz = -1;

// P5 multi-tank UI state
uint8_t s_tank_count = 1;
uint8_t s_sel_tank = 0; // 0-based index; 0xFE = All
char s_tank_names[CATFISH_MAX_TANKS][CATFISH_TANK_NAME_LEN] = {
    "Main", "Tank 2", "Tank 3", "Tank 4"};
lv_obj_t *s_tank_chip_btns[CATFISH_MAX_TANKS + 1] = {nullptr, nullptr, nullptr,
                                                     nullptr, nullptr}; // + "All"

// ------------------------------------------------------------------ v1.9.0
// Home: floating vacation badge + food bar inside the relay card
lv_obj_t *s_lbl_vacation = nullptr;
lv_obj_t *s_food_home_bar = nullptr;
lv_obj_t *s_food_home_lbl = nullptr;
bool s_vacation = false;

// Schedule editor: weekday chips (A1) + per-tank seconds (A2) + presets (A3)
lv_obj_t *s_ed_days_row = nullptr;
lv_obj_t *s_ed_day_btn[7] = {nullptr, nullptr, nullptr, nullptr,
                             nullptr, nullptr, nullptr};
lv_obj_t *s_ed_secs_row = nullptr; // per-tank overrides (multi-tank only)
lv_obj_t *s_ed_sec_sb[CATFISH_MAX_TANKS] = {nullptr, nullptr, nullptr, nullptr};

// History overlay (C1)
lv_obj_t *s_hist_overlay = nullptr;
lv_obj_t *s_hist_lbl = nullptr;

// Settings: policy card (F3) + food card (D1)
lv_obj_t *s_sw_vacation = nullptr;
lv_obj_t *s_sb_gap = nullptr;
lv_obj_t *s_sb_max = nullptr;
lv_obj_t *s_sb_ctch = nullptr;
lv_obj_t *s_sb_low = nullptr;
lv_obj_t *s_food_rows = nullptr;
int s_food_rows_count = -1; // rebuilt when the tank count changes
uint8_t s_food_pct[CATFISH_MAX_TANKS] = {255, 255, 255, 255};
uint16_t s_food_used[CATFISH_MAX_TANKS] = {0, 0, 0, 0};
uint16_t s_food_gps[CATFISH_MAX_TANKS] = {0, 0, 0, 0};
uint16_t s_food_cap[CATFISH_MAX_TANKS] = {0, 0, 0, 0};
uint8_t s_food_low = 20;

// countdown target day offset (v1.9.0 A1: weekday masks push feeds days out)
int s_next_days = 0;

// ---------------------------------------------------------------------------
// Programmatic-input guard (spinbox event storm fix, v1.8.3).
// LVGL 8.4 quirk: lv_spinbox_set_value() ends in lv_textarea_set_text(),
// which fires LV_EVENT_VALUE_CHANGED UNCONDITIONALLY - even when the value
// did not change. Programmatic updates (rebuilds, overlay prefill,
// Home-Assistant-driven ui_set_*) therefore re-enter the spinbox callbacks
// and re-enqueue their UiEvents: an unbounded feedback loop (boot storm on
// the tank-count spinbox; endless NVS writes + MQTT republishes after every
// externally applied pulse). ALL programmatic spinbox writes go through
// spinSetValueQuiet(); user-driven events never run under the guard.
// ---------------------------------------------------------------------------
static bool s_spin_prog = false;

static void spinSetValueQuiet(lv_obj_t *sb, int32_t v) {
  if (!sb) return;
  s_spin_prog = true;
  lv_spinbox_set_value(sb, v);
  s_spin_prog = false;
}

// Backlight idle timeout (M6): auto-dim after this much touch inactivity.
// Touching the screen wakes it again; the user's on/off preference in
// Settings is separate and persisted.
constexpr uint32_t UI_BACKLIGHT_TIMEOUT_MS = 10UL * 60UL * 1000UL;

// ---------------------------------------------------------------------------
// Internal prototypes (order-independent wiring)
// ---------------------------------------------------------------------------
// Shared keyboard behavior (v1.6.1): every editor keyboard follows ONE
// pattern — hidden until a textarea gains focus, rebinds to the focused
// field, scrolls the field above the keyboard, and hides again on the
// keyboard's OK (LV_EVENT_READY) or close (LV_EVENT_CANCEL) key. Clearing
// LV_STATE_FOCUSED on hide makes the next tap re-send FOCUSED (LVGL v8
// re-sends when the state was cleared), so the keyboard comes back.
static void kbHide(lv_obj_t *kb);
static void kbShowFor(lv_obj_t *kb, lv_obj_t *ta);
static void kbStyle(lv_obj_t *kb);
void kbReadyCb(lv_event_t *e);
void kbCancelCb(lv_event_t *e);
void tokenTaFocusCb(lv_event_t *e);
void showScreen(int idx);
void navCb(lv_event_t *e);
void feedBtnCb(lv_event_t *e);
void enableSwitchCb(lv_event_t *e);
void trashCb(lv_event_t *e);
void editorSaveCb(lv_event_t *e);
void editorCancelCb(lv_event_t *e);
void pulseSpinCb(lv_event_t *e);
void blSwitchCb(lv_event_t *e);
void h12SwitchCb(lv_event_t *e);
void relayTestCb(lv_event_t *e);
void rebootCb(lv_event_t *e);
void toastTimerCb(lv_timer_t *t);
void spinMinusCb(lv_event_t *e);
void spinPlusCb(lv_event_t *e);
void rebuildScheduleList();
void openEditor();
void closeEditor();
void addBtnCb(lv_event_t *e);
void updateNextFeedUi();
void buildTokenOverlay();
void openTokenOverlay();
void closeTokenOverlay();
void tokenEditCb(lv_event_t *e);
void tokenSaveCb(lv_event_t *e);
void tokenCancelCb(lv_event_t *e);
void buildMqttOverlay();
void openMqttOverlay();
void closeMqttOverlay();
void mqttEditCb(lv_event_t *e);
void mqttSaveCb(lv_event_t *e);
void mqttCancelCb(lv_event_t *e);
void mqttTaFocusCb(lv_event_t *e);
void buildMechOverlay();
void openMechOverlay();
void closeMechOverlay();
void mechEditCb(lv_event_t *e);
void mechSaveCb(lv_event_t *e);
void mechCancelCb(lv_event_t *e);
void mechRollerCb(lv_event_t *e);
void mechSyncRows();
void buildTankOverlay();
void openTankOverlay(uint8_t tank1);
void closeTankOverlay();
void tankEditCb(lv_event_t *e);
void tankSaveCb(lv_event_t *e);
void tankCancelCb(lv_event_t *e);
void tankTaFocusCb(lv_event_t *e);
void tankCountSpinCb(lv_event_t *e);
void scanTanksCb(lv_event_t *e);
void tankChipCb(lv_event_t *e);
void rebuildTankChips();
void rebuildTankRows();
// v1.9.0
void vacationSwitchCb(lv_event_t *e);
void skipOnceCb(lv_event_t *e);
void dayChipCb(lv_event_t *e);
void presetCb(lv_event_t *e);
void refillCb(lv_event_t *e);
void policySaveCb(lv_event_t *e);
void histBtnCb(lv_event_t *e);
void histCloseCb(lv_event_t *e);
void rebuildFoodRows();
void refreshFoodRows();
const char *fmtDaysMask(uint8_t days, char *buf, size_t cap);

// ---------------------------------------------------------------------------
// Keyboard helpers (v1.6.1) — shared by the token, MQTT and tank editors
// ---------------------------------------------------------------------------
static void kbHide(lv_obj_t *kb) {
  if (!kb) return;
  const lv_obj_t *ta = lv_keyboard_get_textarea(kb);
  if (ta) lv_obj_clear_state((lv_obj_t *)ta, LV_STATE_FOCUSED);
  lv_keyboard_set_textarea(kb, nullptr);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void kbShowFor(lv_obj_t *kb, lv_obj_t *ta) {
  if (!kb || !ta) return;
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_scroll_to_view(ta, LV_ANIM_ON); // focused field above the keyboard
  lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(kb);
}

static void kbStyle(lv_obj_t *kb) {
  lv_obj_set_style_bg_color(kb, lv_color_hex(C_BG_DARK), 0);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(kb, lv_color_hex(C_CARD), LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, lv_color_hex(C_TEXT), LV_PART_ITEMS);
}

void kbReadyCb(lv_event_t *e) {
  kbHide((lv_obj_t *)lv_event_get_target(e)); // OK key
}

void kbCancelCb(lv_event_t *e) {
  kbHide((lv_obj_t *)lv_event_get_target(e)); // keyboard close key
}

// ---------------------------------------------------------------------------
// Small widget builders
// ---------------------------------------------------------------------------
lv_obj_t *makeCard(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, w, h);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 16, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  return card;
}

lv_obj_t *makeTitle(lv_obj_t *parent, const char *txt) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_letter_space(lbl, 2, 0);
  return lbl;
}

lv_obj_t *makeBtn(lv_obj_t *parent, const char *txt, uint32_t bg, uint32_t fg,
                  uint32_t pressed, lv_coord_t w, lv_coord_t h) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_width(btn, (w > 0) ? w : LV_SIZE_CONTENT);
  lv_obj_set_height(btn, h);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(pressed), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_color(lbl, lv_color_hex(fg), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(lbl);
  return btn;
}

lv_obj_t *makeSpinbox(lv_obj_t *parent, int32_t mn, int32_t mx, int32_t val,
                      lv_coord_t w, lv_coord_t h) {
  lv_obj_t *sb = lv_spinbox_create(parent);
  lv_spinbox_set_range(sb, mn, mx);
  lv_spinbox_set_digit_format(sb, 2, 0);
  spinSetValueQuiet(sb, val);
  lv_obj_set_size(sb, w, h);
  lv_obj_set_style_bg_color(sb, lv_color_hex(C_BG_DARK), 0);
  lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(sb, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(sb, &lv_font_montserrat_20, 0);
  lv_obj_set_style_border_color(sb, lv_color_hex(C_CARD_HI), 0);
  lv_obj_set_style_border_width(sb, 1, 0);
  lv_obj_set_style_radius(sb, 8, 0);
  return sb;
}

/** Row of [-] [spinbox] [+] with big touch targets. Returns the spinbox. */
lv_obj_t *makeSpinRow(lv_obj_t *parent, int32_t mn, int32_t mx, int32_t val,
                      lv_event_cb_t on_change) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *sb = makeSpinbox(row, mn, mx, val, 110, 56);
  if (on_change) lv_obj_add_event_cb(sb, on_change, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *minus = makeBtn(row, LV_SYMBOL_MINUS, C_CARD, C_TEXT, C_CARD_HI, 56, 56);
  lv_obj_add_event_cb(minus, spinMinusCb, LV_EVENT_CLICKED, sb);

  lv_obj_t *plus = makeBtn(row, LV_SYMBOL_PLUS, C_CARD, C_TEXT, C_CARD_HI, 56, 56);
  lv_obj_add_event_cb(plus, spinPlusCb, LV_EVENT_CLICKED, sb);

  return sb;
}

void buildRollerOptions() {
  // Hour options: the roller's SELECTED INDEX is always the 24-hour hour
  // value (editorSaveCb uses it directly) — only the LABELS change per format.
  s_hour_opts[0] = '\0';
  char b[12];
  for (int i = 0; i < 24; i++) {
    if (s_hour12) {
      int h12 = i % 12;
      if (h12 == 0) h12 = 12;
      snprintf(b, sizeof(b), "%d %s", h12, i < 12 ? "AM" : "PM");
    } else {
      snprintf(b, sizeof(b), "%02d", i);
    }
    strcat(s_hour_opts, b);
    if (i < 23) strcat(s_hour_opts, "\n");
  }
  s_min_opts[0] = '\0';
  for (int i = 0; i < 60; i++) {
    char b[4];
    snprintf(b, sizeof(b), "%02d", i);
    strcat(s_min_opts, b);
    if (i < 59) strcat(s_min_opts, "\n");
  }
}

// ---------------------------------------------------------------------------
// Chrome: status bar, nav bar, content area (per screen)
// ---------------------------------------------------------------------------
void addStatusBar(lv_obj_t *parent, int idx) {
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, lv_pct(100), 48);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(C_BG_DARK), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(bar);
  lv_label_set_text(title, "CATFISH FEEDER");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_letter_space(title, 3, 0);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 16, 0);

  // v1.9.0 (A5): floating vacation badge on the HOME screen only (absolute —
  // outside the flex flow so the centered Home layout is untouched)
  if (idx == 0) {
    s_lbl_vacation = lv_label_create(parent);
    lv_label_set_text(s_lbl_vacation, "VACATION \xE2\x80\xA2 SCHEDULES PAUSED");
    lv_obj_set_style_text_font(s_lbl_vacation, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_vacation, lv_color_hex(C_RED), 0);
    lv_obj_align(s_lbl_vacation, LV_ALIGN_TOP_RIGHT, -14, 52);
    lv_obj_add_flag(s_lbl_vacation, LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_t *wf = lv_label_create(bar);
  lv_label_set_text(wf, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_font(wf, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(wf, lv_color_hex(C_MUTED), 0);
  lv_obj_align(wf, LV_ALIGN_RIGHT_MID, -68, 0);
  s_wifi_sym[idx] = wf;

  lv_obj_t *ck = lv_label_create(bar);
  lv_label_set_text(ck, "--:--");
  lv_obj_set_style_text_font(ck, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(ck, lv_color_hex(C_TEXT), 0);
  lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -16, 0);
  s_clock_lbl[idx] = ck;
}

void addNavBar(lv_obj_t *parent, int scr_idx) {
  static const char *defs[3] = {
      LV_SYMBOL_HOME "  Home",
      LV_SYMBOL_LIST "  Schedule",
      LV_SYMBOL_SETTINGS "  Settings"};

  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, lv_pct(100), 72);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(C_BG_DARK), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  for (int b = 0; b < 3; b++) {
    lv_obj_t *btn = makeBtn(bar, defs[b], C_CARD, C_MUTED, C_CARD_HI, 244, 64);
    lv_obj_add_event_cb(btn, navCb, LV_EVENT_CLICKED, (void *)(intptr_t)b);
    s_nav_btn[scr_idx][b] = btn;
  }
}

lv_obj_t *addContent(lv_obj_t *parent) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, lv_pct(100), 360); // 480 - 48 (status) - 72 (nav)
  lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_style_pad_all(c, 16, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 12, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
void showScreen(int idx) {
  lv_scr_load(s_scr[idx]);
  for (int s = 0; s < 3; s++) {
    for (int b = 0; b < 3; b++) {
      const bool act = (b == idx);
      lv_obj_set_style_bg_color(s_nav_btn[s][b],
                                lv_color_hex(act ? C_CARD_HI : C_CARD), 0);
      lv_obj_set_style_text_color(s_nav_btn[s][b],
                                  lv_color_hex(act ? C_AMBER : C_MUTED), 0);
    }
  }
}

void navCb(lv_event_t *e) {
  closeEditor(); // never leave the schedule editor open across screens
  showScreen((int)(intptr_t)lv_event_get_user_data(e));
}

void feedBtnCb(lv_event_t *e) {
  (void)e;
  ui_request_feed(prefs::getDefaultPulse());
}

void enableSwitchCb(lv_event_t *e) {
  // v1.8.2: pack the entry IDENTITY (h<<12|m<<6|s), not the list index —
  // the index can go stale while the event sits in the queue (a dashboard
  // replaceAll or a trash tap processed in the same drain window shifts
  // entries). Identity stays valid; matches trashCb / removeByIdentity.
  const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  const uint8_t h = (uint8_t)((id >> 12) & 0x1F);
  const uint8_t m = (uint8_t)((id >> 6) & 0x3F);
  const uint8_t s = (uint8_t)(id & 0x3F);
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  const bool en = lv_obj_has_state(sw, LV_STATE_CHECKED);
  // H3: the scheduler is single-writer (loop() context only) — the LVGL task
  // just reports identity + target state; loop() applies it and flushes NVS.
  UiEvent ev;
  ev.type = UI_EVT_SCHED_PERSIST;
  ev.a = h;
  ev.b = m;
  ev.c = s;
  ev.flag = en;
  uiEventSend(ev);
  Serial.printf("[UI] schedule entry %02u:%02u (%us) -> %s (queued)\n",
                (unsigned)h, (unsigned)m, (unsigned)s, en ? "enabled" : "disabled");
}

void trashCb(lv_event_t *e) {
  // user_data packs the entry identity (h<<12 | m<<6 | s) so a tap is immune
  // to index shifts caused by other pending removals; loop() does the actual
  // feed_scheduler::removeByIdentity() + list rebuild under the LVGL lock.
  const uintptr_t packed = (uintptr_t)(intptr_t)lv_event_get_user_data(e);
  UiEvent ev;
  ev.type = UI_EVT_SCHED_REMOVE;
  ev.a = (uint8_t)((packed >> 12) & 0x1F);
  ev.b = (uint8_t)((packed >> 6) & 0x3F);
  ev.c = (uint8_t)(packed & 0x3F);
  uiEventSend(ev);
}

void addBtnCb(lv_event_t *e) {
  (void)e;
  openEditor();
}

// ---------------------------------------------------------------------------
// v1.9.0 feeding-feature callbacks (all enqueue; loop() applies + persists)
// ---------------------------------------------------------------------------
/** "Daily" / "Mon-Fri" / "Sat+Sun" / "M,T,F" summary for a weekday mask. */
const char *fmtDaysMask(uint8_t days, char *buf, size_t cap) {
  static const char *NAMES[7] = {"M", "T", "W", "T", "F", "S", "S"};
  if (days == 0 || days == 0x7F) {
    snprintf(buf, cap, "Daily");
    return buf;
  }
  if (days == 0x1F) {
    snprintf(buf, cap, "Mon-Fri");
    return buf;
  }
  if (days == 0x60) {
    snprintf(buf, cap, "Sat+Sun");
    return buf;
  }
  int n = 0;
  for (int i = 0; i < 7; i++) {
    if (days & (1u << i)) {
      n += snprintf(buf + n, (n < (int)cap) ? (cap - n) : 0, "%s%s", n ? "," : "",
                    NAMES[i]);
    }
  }
  return buf;
}

void vacationSwitchCb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  UiEvent ev;
  ev.type = UI_EVT_VACATION;
  ev.flag = on;
  uiEventSend(ev);
  Serial.printf("[UI] vacation -> %s (queued)\n", on ? "ON" : "OFF");
}

void skipOnceCb(lv_event_t *e) {
  // user_data packs the HH:MM identity (h<<8 | m); skip-once, then cleared.
  const uintptr_t packed = (uintptr_t)(intptr_t)lv_event_get_user_data(e);
  UiEvent ev;
  ev.type = UI_EVT_SKIP_NEXT;
  ev.a = (uint8_t)((packed >> 8) & 0x1F);
  ev.b = (uint8_t)(packed & 0x3F);
  uiEventSend(ev);
}

void dayChipCb(lv_event_t *e) {
  // visual toggle only; editorSaveCb reads the CHECKED state of every chip
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  const bool on = lv_obj_has_state(btn, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(btn, lv_color_hex(on ? C_AMBER : C_CARD), 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(on ? C_INK : C_MUTED), 0);
}

void presetCb(lv_event_t *e) {
  // A3: S/M/L portion presets -> shared seconds (per-tank overrides untouched)
  const uintptr_t secs = (uintptr_t)(intptr_t)lv_event_get_user_data(e);
  if (s_sb_sec) spinSetValueQuiet(s_sb_sec, (int32_t)secs);
}

void refillCb(lv_event_t *e) {
  UiEvent ev;
  ev.type = UI_EVT_REFILL;
  ev.a = (uint8_t)(uintptr_t)lv_event_get_user_data(e); // tank 1..4 or 0xFF
  uiEventSend(ev);
}

void policySaveCb(lv_event_t *e) {
  (void)e;
  UiEvent ev;
  ev.type = UI_EVT_GUARD_CFG;
  if (s_sb_gap) ev.a = (uint8_t)lv_spinbox_get_value(s_sb_gap);
  if (s_sb_max) ev.b = (uint8_t)lv_spinbox_get_value(s_sb_max);
  if (s_sb_ctch) ev.c = (uint8_t)lv_spinbox_get_value(s_sb_ctch);
  if (s_sb_low) ev.d = (uint8_t)lv_spinbox_get_value(s_sb_low);
  uiEventSend(ev); // loop() persists + refreshes the food card
}

void histBtnCb(lv_event_t *e) {
  (void)e;
  if (!s_hist_overlay) return;
  lv_obj_clear_flag(s_hist_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_hist_overlay);
}

void histCloseCb(lv_event_t *e) {
  (void)e;
  if (s_hist_overlay) lv_obj_add_flag(s_hist_overlay, LV_OBJ_FLAG_HIDDEN);
}

void editorSaveCb(lv_event_t *e) {
  (void)e;
  UiEvent ev;
  ev.type = UI_EVT_SCHED_ADD;
  ev.a = (uint8_t)lv_roller_get_selected(s_r_hour);
  ev.b = (uint8_t)lv_roller_get_selected(s_r_min);
  ev.c = (uint8_t)lv_spinbox_get_value(s_sb_sec);
  // P5: pack target tanks from the editor checkboxes (missing checkboxes =
  // single-tank setup = tank 1). Default: every checkbox unchecked row would
  // mean "no tank" -> loop falls back to tank 1 via effectiveTankMask.
  uint8_t mask = 0;
  for (int i = 0; i < CATFISH_MAX_TANKS; i++) {
    if (s_ed_tank_cb[i] && lv_obj_has_state(s_ed_tank_cb[i], LV_STATE_CHECKED))
      mask |= (uint8_t)(1u << i);
  }
  ev.tankMask = mask;
  // v1.9.0 (A1): weekday mask from the day chips (none checked = daily).
  uint8_t days = 0;
  for (int i = 0; i < 7; i++) {
    if (s_ed_day_btn[i] && lv_obj_has_state(s_ed_day_btn[i], LV_STATE_CHECKED))
      days |= (uint8_t)(1u << i);
  }
  ev.days = days;
  // v1.9.0 (A2): per-tank pulse overrides (0 = inherit the shared seconds).
  for (int i = 0; i < CATFISH_MAX_TANKS; i++) {
    ev.secTank[i] = s_ed_sec_sb[i] ? (uint8_t)lv_spinbox_get_value(s_ed_sec_sb[i]) : 0;
  }
  // loop() performs the add: on success it rebuilds the list, closes the
  // editor and toasts; on failure the editor stays open with the reason.
  uiEventSend(ev);
}

void editorCancelCb(lv_event_t *e) {
  (void)e;
  closeEditor();
}

void pulseSpinCb(lv_event_t *e) {
  if (s_spin_prog) return; // programmatic ui_set_pulse() replay (see guard note)
  lv_obj_t *sb = (lv_obj_t *)lv_event_get_target(e);
  UiEvent ev;
  ev.type = UI_EVT_SET_PULSE;
  ev.a = (uint8_t)lv_spinbox_get_value(sb);
  uiEventSend(ev); // loop() persists (keeps flash writes out of the LVGL task)
}

/** Apply the effective backlight = user preference AND not idle-dimmed. */
void applyBacklight() {
  if (s_bl_cb) s_bl_cb(s_bl_on && !s_dimmed);
}

void blSwitchCb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  s_bl_on = on;
  s_dimmed = false; // a manual toggle always overrides an idle dim
  applyBacklight();
  UiEvent ev;
  ev.type = UI_EVT_SET_BL;
  ev.flag = on;
  uiEventSend(ev); // loop() persists the preference
  Serial.printf("[UI] backlight %s\n", on ? "ON" : "OFF");
}

void h12SwitchCb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  const bool h12 = lv_obj_has_state(sw, LV_STATE_CHECKED);
  UiEvent ev;
  ev.type = UI_EVT_SET_FMT12;
  ev.flag = h12;
  uiEventSend(ev); // loop() persists + re-renders dependent labels
  Serial.printf("[UI] clock format -> %s\n", h12 ? "12-hour" : "24-hour");
}

void relayTestCb(lv_event_t *e) {
  (void)e;
  UiEvent ev;
  ev.type = UI_EVT_RELAY_TEST;
  uiEventSend(ev); // loop() enqueues the (blocking) probe on the net worker
}

void rebootCb(lv_event_t *e) {
  (void)e;
  UiEvent ev;
  ev.type = UI_EVT_REBOOT;
  uiEventSend(ev); // loop() performs the reboot
}

void tokenEditCb(lv_event_t *e) {
  (void)e;
  openTokenOverlay();
}

void tokenCancelCb(lv_event_t *e) {
  (void)e;
  closeTokenOverlay();
}

void tokenSaveCb(lv_event_t *e) {
  (void)e;
  const char *t = lv_textarea_get_text(s_tok_ta);
  if (!t || t[0] == '\0' || strlen(t) >= CATFISH_TOKEN_LEN) {
    ui_toast("Token: 1-23 characters");
    return;
  }
  UiEvent ev;
  ev.type = UI_EVT_SET_TOKEN;
  ev.flag = s_tok_rotate && lv_obj_has_state(s_tok_rotate, LV_STATE_CHECKED);
  strlcpy(ev.token, t, sizeof(ev.token));
  if (uiEventSend(ev)) closeTokenOverlay(); // loop() persists (+rotates relay)
}

// ---------------------------------------------------------------------------
// MQTT editor overlay (same pattern as the token editor; the keyboard is
// hidden until a textarea is focused, then rebinds to whichever field has
// focus — LVGL scrolls the focused field above the keyboard automatically).
// ---------------------------------------------------------------------------
void mqttEditCb(lv_event_t *e) {
  (void)e;
  openMqttOverlay();
}

void mqttCancelCb(lv_event_t *e) {
  (void)e;
  closeMqttOverlay();
}

void mqttTaFocusCb(lv_event_t *e) {
  kbShowFor(s_mq_kb, (lv_obj_t *)lv_event_get_target(e));
}

void mqttSaveCb(lv_event_t *e) {
  (void)e;
  const bool en = s_mq_sw_en && lv_obj_has_state(s_mq_sw_en, LV_STATE_CHECKED);
  const char *host = lv_textarea_get_text(s_mq_ta_host);
  if (en && (!host || host[0] == '\0')) {
    ui_toast("Broker host required");
    return;
  }
  const char *prefix = lv_textarea_get_text(s_mq_ta_prefix);
  if (!prefix || prefix[0] == '\0' || strchr(prefix, '/') || strchr(prefix, ' ') ||
      strchr(prefix, '+') || strchr(prefix, '#')) {
    ui_toast("Prefix: no spaces or wildcards");
    return;
  }
  UiEvent ev;
  ev.type = UI_EVT_MQTT_CFG;
  ev.mqttEnable = en;
  ev.mqttPort = (uint16_t)lv_spinbox_get_value(s_mq_sb_port);
  strlcpy(ev.mqttHost, host ? host : "", sizeof(ev.mqttHost));
  strlcpy(ev.mqttUser, lv_textarea_get_text(s_mq_ta_user), sizeof(ev.mqttUser));
  strlcpy(ev.mqttPass, lv_textarea_get_text(s_mq_ta_pass), sizeof(ev.mqttPass));
  strlcpy(ev.mqttPrefix, prefix ? prefix : "", sizeof(ev.mqttPrefix));
  if (uiEventSend(ev)) closeMqttOverlay(); // loop() persists + applies
}

void toastTimerCb(lv_timer_t *t) {
  (void)t;
  if (s_toast_box) lv_obj_add_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
  if (s_toast_tmr) lv_timer_pause(s_toast_tmr);
}

// ---------------------------------------------------------------------------
// Mechanism editor overlay (P4). Roller selects motor/servo/stepper; the
// servo and stepper parameter rows appear only for their mechanism. Pure
// LVGL widgets (roller + spinboxes + switch) — no keyboard needed.
// ---------------------------------------------------------------------------
void mechEditCb(lv_event_t *e) {
  (void)e;
  openMechOverlay();
}

void mechCancelCb(lv_event_t *e) {
  (void)e;
  closeMechOverlay();
}

uint8_t mechRollerToId(uint16_t sel) {
  // roller options order matches CATFISH_MECH_* (motor, servo, stepper)
  return (uint8_t)sel;
}

void mechRollerCb(lv_event_t *e) {
  (void)e;
  mechSyncRows();
}

void mechSyncRows() {
  if (!s_mech_roller) return;
  const uint8_t id = mechRollerToId(lv_roller_get_selected(s_mech_roller));
  if (s_mech_row_servo) {
    if (id == CATFISH_MECH_SERVO)
      lv_obj_clear_flag(s_mech_row_servo, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_mech_row_servo, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_mech_row_step) {
    if (id == CATFISH_MECH_STEPPER)
      lv_obj_clear_flag(s_mech_row_step, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_mech_row_step, LV_OBJ_FLAG_HIDDEN);
  }
}

void mechSaveCb(lv_event_t *e) {
  (void)e;
  if (!s_mech_roller) return;
  UiEvent ev;
  ev.type = UI_EVT_MECH_CFG;
  ev.mechId = mechRollerToId(lv_roller_get_selected(s_mech_roller));
  if (s_mech_sb_rest) ev.mechRest = (uint8_t)lv_spinbox_get_value(s_mech_sb_rest);
  if (s_mech_sb_feed) ev.mechFeed = (uint8_t)lv_spinbox_get_value(s_mech_sb_feed);
  if (s_mech_sb_hz) ev.mechHz = (uint16_t)lv_spinbox_get_value(s_mech_sb_hz);
  if (s_mech_sw_dir)
    ev.mechDir = lv_obj_has_state(s_mech_sw_dir, LV_STATE_CHECKED) ? 1 : 0;
  if (uiEventSend(ev)) {
    closeMechOverlay();
    ui_toast("Updating mechanism..."); // RES_MECH reports the outcome
  }
}

void spinMinusCb(lv_event_t *e) {
  lv_spinbox_decrement((lv_obj_t *)lv_event_get_user_data(e));
}

void spinPlusCb(lv_event_t *e) {
  lv_spinbox_increment((lv_obj_t *)lv_event_get_user_data(e));
}

// ---------------------------------------------------------------------------
// Schedule list + editor
// ---------------------------------------------------------------------------
void rebuildScheduleList() {
  if (!s_sched_list) return;
  lv_obj_clean(s_sched_list);

  const int n = feed_scheduler::getCount();
  if (n == 0) {
    lv_obj_t *empty = lv_label_create(s_sched_list);
    lv_label_set_text(empty, "No feeding times yet. Tap ADD FEEDING TIME below.");
    lv_obj_set_style_text_color(empty, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
    return;
  }

  const FeedTime *all = feed_scheduler::getAll();
  for (int i = 0; i < n; i++) {
    const FeedTime &en = all[i];

    lv_obj_t *card = lv_obj_create(s_sched_list);
    lv_obj_set_size(card, lv_pct(100), 76);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_ver(card, 6, 0);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    char buf[96];
    char hm[12];
    char day[24];
    catfishFormatHm(hm, sizeof(hm), en.hour, en.minute, s_hour12);
    fmtDaysMask(en.days, day, sizeof(day));
    // v1.9.0 (A2): per-tank overrides replace the plain tank tag when set.
    bool hasOverrides = false;
    char secsTag[40] = {0};
    int sl = 0;
    for (uint8_t b = 0; b < s_tank_count && b < CATFISH_MAX_TANKS; b++) {
      if (en.secTank[b]) {
        hasOverrides = true;
        sl += snprintf(secsTag + sl, sizeof(secsTag) - sl, "%sT%u:%us",
                       sl ? "+" : "", (unsigned)(b + 1),
                       (unsigned)en.secTank[b]);
      }
    }
    if (s_tank_count > 1) {
      if (hasOverrides) {
        snprintf(buf, sizeof(buf), "%s  \xE2\x80\xA2  %u s  \xE2\x80\xA2  %s  \xE2\x80\xA2  %s",
                 hm, (unsigned)en.seconds, secsTag, day);
      } else {
        char tag[16] = {0};
        int tl = 0;
        for (uint8_t b = 0; b < s_tank_count && b < CATFISH_MAX_TANKS; b++) {
          if (en.tanks & (1u << b)) {
            tl += snprintf(tag + tl, sizeof(tag) - tl, "%s%u", tl ? "+" : "",
                           (unsigned)(b + 1));
          }
        }
        snprintf(buf, sizeof(buf), "%s  \xE2\x80\xA2  %u s  \xE2\x80\xA2  T%s  \xE2\x80\xA2  %s",
                 hm, (unsigned)en.seconds, tag[0] ? tag : "1", day);
      }
    } else {
      snprintf(buf, sizeof(buf), "%s  \xE2\x80\xA2  %u s  \xE2\x80\xA2  %s", hm,
               (unsigned)en.seconds, day);
    }
    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, buf);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(tl,
                                lv_color_hex(en.enabled ? C_TEXT : C_MUTED), 0);
    lv_obj_set_flex_grow(tl, 1);

    // v1.9.0 (E4): skip-this-entry-once button
    lv_obj_t *skip = lv_btn_create(card);
    lv_obj_set_size(skip, 56, 56);
    lv_obj_set_style_radius(skip, 12, 0);
    lv_obj_set_style_bg_color(skip, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(skip, lv_color_hex(C_CARD_HI), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(skip, 0, 0);
    lv_obj_set_style_shadow_width(skip, 0, 0);
    lv_obj_t *si = lv_label_create(skip);
    lv_label_set_text(si, LV_SYMBOL_MINUS); // "skip once"
    lv_obj_set_style_text_color(si, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(si, &lv_font_montserrat_20, 0);
    lv_obj_center(si);
    lv_obj_add_event_cb(skip, skipOnceCb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)((en.hour << 8) | en.minute));

    lv_obj_t *sw = lv_switch_create(card);
    lv_obj_set_size(sw, 72, 36);
    if (en.enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, enableSwitchCb, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)((en.hour << 12) | (en.minute << 6) |
                                            en.seconds));

    lv_obj_t *trash = lv_btn_create(card);
    lv_obj_set_size(trash, 64, 64);
    lv_obj_set_style_radius(trash, 12, 0);
    lv_obj_set_style_bg_color(trash, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(trash, lv_color_hex(C_CARD_HI), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(trash, 0, 0);
    lv_obj_set_style_shadow_width(trash, 0, 0);
    lv_obj_t *ti = lv_label_create(trash);
    lv_label_set_text(ti, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(ti, lv_color_hex(C_RED), 0);
    lv_obj_set_style_text_font(ti, &lv_font_montserrat_20, 0);
    lv_obj_center(ti);
    lv_obj_add_event_cb(trash, trashCb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)((en.hour << 12) | (en.minute << 6) |
                                            en.seconds));
  }
}

void openEditor() {
  if (!s_editor) return;
  lv_roller_set_selected(s_r_hour, 7, LV_ANIM_OFF);
  lv_roller_set_selected(s_r_min, 30, LV_ANIM_OFF);
  spinSetValueQuiet(s_sb_sec, prefs::getDefaultPulse());

  // v1.9.0 (A1): all day chips checked = daily (the default)
  for (int i = 0; i < 7; i++) {
    if (s_ed_day_btn[i]) {
      lv_obj_add_state(s_ed_day_btn[i], LV_STATE_CHECKED);
      lv_obj_set_style_bg_color(s_ed_day_btn[i], lv_color_hex(C_AMBER), 0);
      lv_obj_set_style_text_color(s_ed_day_btn[i], lv_color_hex(C_INK), 0);
    }
  }
  // v1.9.0 (A2): clear the per-tank pulse overrides
  for (int i = 0; i < CATFISH_MAX_TANKS; i++) {
    if (s_ed_sec_sb[i]) spinSetValueQuiet(s_ed_sec_sb[i], 0);
  }
  // v1.9.0 (A2): show the per-tank row only in multi-tank setups
  if (s_ed_secs_row) {
    if (s_tank_count > 1)
      lv_obj_clear_flag(s_ed_secs_row, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_ed_secs_row, LV_OBJ_FLAG_HIDDEN);
  }

  // P5: rebuild the tank checkbox column for the current count (all checked
  // = "feed every configured tank"; hidden entirely for a single tank).
  if (s_ed_tank_row) {
    lv_obj_clean(s_ed_tank_row);
    for (int i = 0; i < CATFISH_MAX_TANKS; i++) s_ed_tank_cb[i] = nullptr;
    if (s_tank_count > 1) {
      lv_obj_t *lt = lv_label_create(s_ed_tank_row);
      lv_label_set_text(lt, "TANKS");
      lv_obj_set_style_text_font(lt, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(lt, lv_color_hex(C_MUTED), 0);
      for (uint8_t i = 0; i < s_tank_count && i < CATFISH_MAX_TANKS; i++) {
        lv_obj_t *cb = lv_checkbox_create(s_ed_tank_row);
        lv_checkbox_set_text(cb, s_tank_names[i]);
        lv_obj_add_state(cb, LV_STATE_CHECKED);
        lv_obj_set_style_text_font(cb, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(cb, lv_color_hex(C_TEXT), 0);
        s_ed_tank_cb[i] = cb;
      }
    }
  }

  lv_obj_clear_flag(s_editor, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_editor);
}

void closeEditor() {
  if (s_editor) lv_obj_add_flag(s_editor, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *makeRoller(lv_obj_t *parent, const char *opts, int sel) {
  lv_obj_t *r = lv_roller_create(parent);
  lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(r, 4);
  lv_roller_set_selected(r, sel, LV_ANIM_OFF);
  lv_obj_set_width(r, 150);
  lv_obj_set_style_bg_color(r, lv_color_hex(C_BG_DARK), 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(r, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_color(r, lv_color_hex(C_INK), LV_PART_SELECTED);
  lv_obj_set_style_bg_color(r, lv_color_hex(C_AMBER), LV_PART_SELECTED);
  lv_obj_set_style_text_font(r, &lv_font_montserrat_20, LV_PART_SELECTED);
  lv_obj_set_style_border_color(r, lv_color_hex(C_CARD_HI), 0);
  lv_obj_set_style_border_width(r, 1, 0);
  return r;
}

// ---------------------------------------------------------------------------
// Screen builders
// ---------------------------------------------------------------------------
void buildHome() {
  s_scr[0] = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_scr[0], lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_scr[0], LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr[0], LV_OBJ_FLAG_SCROLLABLE);

  addStatusBar(s_scr[0], 0);
  addNavBar(s_scr[0], 0);
  lv_obj_t *c = addContent(s_scr[0]);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // FEED NOW — big amber action button
  lv_obj_t *feed = lv_btn_create(c);
  s_feed_btn = feed;
  lv_obj_set_size(feed, 420, 120);
  lv_obj_set_style_radius(feed, 24, 0);
  lv_obj_set_style_bg_color(feed, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_bg_color(feed, lv_color_hex(C_AMBER_HI), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(feed, lv_color_hex(C_CARD), LV_STATE_DISABLED);
  lv_obj_set_style_bg_opa(feed, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(feed, 0, 0);
  lv_obj_set_style_shadow_width(feed, 0, 0);
  lv_obj_t *fl = lv_label_create(feed);
  lv_label_set_text(fl, LV_SYMBOL_PLAY "  FEED NOW");
  lv_obj_set_style_text_font(fl, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(fl, lv_color_hex(C_INK), 0);
  lv_obj_set_style_text_color(fl, lv_color_hex(C_MUTED), LV_STATE_DISABLED);
  lv_obj_center(fl);
  lv_obj_add_event_cb(feed, feedBtnCb, LV_EVENT_CLICKED, nullptr);

  // P5: tank selection chips (populated by ui_tanks_rebuild; hidden at count 1)
  s_tank_chips = lv_obj_create(c);
  lv_obj_remove_style_all(s_tank_chips);
  lv_obj_set_size(s_tank_chips, 420, 56);
  lv_obj_set_flex_flow(s_tank_chips, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_tank_chips, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s_tank_chips, 8, 0);
  lv_obj_clear_flag(s_tank_chips, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_tank_chips, LV_OBJ_FLAG_HIDDEN);

  // info cards row: next feeding / last fed / relay unit
  lv_obj_t *row = lv_obj_create(c);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 170);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  // Next feeding card
  lv_obj_t *card_next = makeCard(row, lv_pct(32), 170);
  lv_obj_set_flex_flow(card_next, LV_FLEX_FLOW_COLUMN);
  makeTitle(card_next, "NEXT FEEDING");
  s_next_value = lv_label_create(card_next);
  lv_label_set_text(s_next_value, "--:--");
  lv_obj_set_style_text_font(s_next_value, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_next_value, lv_color_hex(C_TEXT), 0);
  lv_obj_set_width(s_next_value, lv_pct(100));
  lv_label_set_long_mode(s_next_value, LV_LABEL_LONG_DOT);
  s_next_sub = lv_label_create(card_next);
  lv_label_set_text(s_next_sub, "");
  lv_obj_set_style_text_font(s_next_sub, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_next_sub, lv_color_hex(C_MUTED), 0);
  lv_obj_set_width(s_next_sub, lv_pct(100));
  lv_label_set_long_mode(s_next_sub, LV_LABEL_LONG_DOT);

  // Last fed card
  lv_obj_t *card_last = makeCard(row, lv_pct(32), 170);
  lv_obj_set_flex_flow(card_last, LV_FLEX_FLOW_COLUMN);
  makeTitle(card_last, "LAST FED");
  s_last_value = lv_label_create(card_last);
  lv_label_set_text(s_last_value, "Never");
  lv_obj_set_style_text_font(s_last_value, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_last_value, lv_color_hex(C_TEXT), 0);
  lv_obj_set_width(s_last_value, lv_pct(100));
  lv_label_set_long_mode(s_last_value, LV_LABEL_LONG_DOT);

  // Relay unit card
  lv_obj_t *card_relay = makeCard(row, lv_pct(32), 170);
  lv_obj_set_flex_flow(card_relay, LV_FLEX_FLOW_COLUMN);
  makeTitle(card_relay, "RELAY UNIT");
  lv_obj_t *rrow = lv_obj_create(card_relay);
  lv_obj_remove_style_all(rrow);
  lv_obj_set_size(rrow, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(rrow, 8, 0);
  lv_obj_clear_flag(rrow, LV_OBJ_FLAG_SCROLLABLE);

  s_relay_dot = lv_obj_create(rrow);
  lv_obj_remove_style_all(s_relay_dot);
  lv_obj_set_size(s_relay_dot, 14, 14);
  lv_obj_set_style_radius(s_relay_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_relay_dot, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_bg_opa(s_relay_dot, LV_OPA_COVER, 0);

  s_relay_txt = lv_label_create(rrow);
  lv_label_set_text(s_relay_txt, "Searching...");
  lv_obj_set_style_text_font(s_relay_txt, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_relay_txt, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_grow(s_relay_txt, 1);
  lv_label_set_long_mode(s_relay_txt, LV_LABEL_LONG_WRAP);

  // v1.9.0 (D1): food-level bar for the selected tank (hidden until a hopper
  // capacity is configured — the estimator needs "hopper size" in Settings)
  lv_obj_t *frow = lv_obj_create(card_relay);
  lv_obj_remove_style_all(frow);
  lv_obj_set_size(frow, lv_pct(100), 30);
  lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(frow, 8, 0);
  lv_obj_clear_flag(frow, LV_OBJ_FLAG_SCROLLABLE);
  s_food_home_bar = lv_bar_create(frow);
  lv_obj_set_size(s_food_home_bar, lv_pct(70), 14);
  lv_bar_set_range(s_food_home_bar, 0, 100);
  lv_bar_set_value(s_food_home_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s_food_home_bar, lv_color_hex(C_BG_DARK), 0);
  lv_obj_set_style_bg_color(s_food_home_bar, lv_color_hex(C_AMBER), LV_PART_INDICATOR);
  s_food_home_lbl = lv_label_create(frow);
  lv_label_set_text(s_food_home_lbl, "Food: --");
  lv_obj_set_style_text_font(s_food_home_lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_food_home_lbl, lv_color_hex(C_MUTED), 0);
}

void buildSchedule() {
  s_scr[1] = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_scr[1], lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_scr[1], LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr[1], LV_OBJ_FLAG_SCROLLABLE);

  addStatusBar(s_scr[1], 1);
  addNavBar(s_scr[1], 1);
  lv_obj_t *c = addContent(s_scr[1]);

  // scrollable list of feed-time cards (grows to fill remaining height)
  s_sched_list = lv_obj_create(c);
  lv_obj_remove_style_all(s_sched_list);
  lv_obj_set_flex_grow(s_sched_list, 1);
  lv_obj_set_width(s_sched_list, lv_pct(100));
  lv_obj_set_flex_flow(s_sched_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_sched_list, 12, 0);

  // v1.9.0 (C1) + ADD: button row (pinned below the list)
  lv_obj_t *btnrow = lv_obj_create(c);
  lv_obj_remove_style_all(btnrow);
  lv_obj_set_width(btnrow, lv_pct(100));
  lv_obj_set_height(btnrow, 64);
  lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(btnrow, 12, 0);
  lv_obj_clear_flag(btnrow, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *hist = makeBtn(btnrow, LV_SYMBOL_LIST "  HISTORY", C_CARD,
                           C_TEXT, C_CARD_HI, lv_pct(34), 64);
  lv_obj_add_event_cb(hist, histBtnCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *add = makeBtn(btnrow, LV_SYMBOL_PLUS "  ADD FEEDING TIME", C_CARD,
                          C_AMBER, C_CARD_HI, lv_pct(66), 64);
  lv_obj_set_style_border_color(add, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_border_width(add, 2, 0);
  lv_obj_add_event_cb(add, addBtnCb, LV_EVENT_CLICKED, nullptr);

  rebuildScheduleList();

  // --- editor overlay (hidden) ---
  s_editor = lv_obj_create(s_scr[1]);
  lv_obj_remove_style_all(s_editor);
  lv_obj_set_size(s_editor, lv_pct(100), 360);
  lv_obj_align(s_editor, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_style_bg_color(s_editor, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_editor, LV_OPA_90, 0);
  lv_obj_add_flag(s_editor, LV_OBJ_FLAG_CLICKABLE); // swallow clicks behind
  lv_obj_add_flag(s_editor, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *card = makeCard(s_editor, 736, 336);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 8, 0);
  lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE); // v1.9.0: richer editor scrolls
  lv_obj_set_scroll_dir(card, LV_DIR_VER);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "NEW FEEDING TIME");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_text_letter_space(title, 2, 0);

  lv_obj_t *editors = lv_obj_create(card);
  lv_obj_remove_style_all(editors);
  lv_obj_set_width(editors, lv_pct(100));
  lv_obj_set_height(editors, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(editors, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(editors, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(editors, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *col_h = lv_obj_create(editors);
  lv_obj_remove_style_all(col_h);
  lv_obj_set_size(col_h, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col_h, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col_h, 6, 0);
  lv_obj_t *lh = lv_label_create(col_h);
  lv_label_set_text(lh, "HOUR");
  lv_obj_set_style_text_font(lh, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lh, lv_color_hex(C_MUTED), 0);
  s_r_hour = makeRoller(col_h, s_hour_opts, 7);

  lv_obj_t *col_m = lv_obj_create(editors);
  lv_obj_remove_style_all(col_m);
  lv_obj_set_size(col_m, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col_m, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col_m, 6, 0);
  lv_obj_t *lm = lv_label_create(col_m);
  lv_label_set_text(lm, "MINUTE");
  lv_obj_set_style_text_font(lm, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lm, lv_color_hex(C_MUTED), 0);
  s_r_min = makeRoller(col_m, s_min_opts, 30);

  lv_obj_t *col_s = lv_obj_create(editors);
  lv_obj_remove_style_all(col_s);
  lv_obj_set_size(col_s, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col_s, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col_s, 6, 0);
  lv_obj_t *ls = lv_label_create(col_s);
  lv_label_set_text(ls, "SECONDS");
  lv_obj_set_style_text_font(ls, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(ls, lv_color_hex(C_MUTED), 0);
  s_sb_sec = makeSpinRow(col_s, 1, 30, prefs::getDefaultPulse(), nullptr);

  // v1.9.0 (A3): portion presets S/M/L under the seconds spinbox
  lv_obj_t *preset_row = lv_obj_create(col_s);
  lv_obj_remove_style_all(preset_row);
  lv_obj_set_size(preset_row, LV_SIZE_CONTENT, 40);
  lv_obj_set_flex_flow(preset_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(preset_row, 6, 0);
  lv_obj_clear_flag(preset_row, LV_OBJ_FLAG_SCROLLABLE);
  static const char *PRESETS[3] = {"S", "M", "L"};
  static const int32_t PRESET_S[3] = {2, 5, 10};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *p = makeBtn(preset_row, PRESETS[i], C_CARD, C_MUTED, C_CARD_HI,
                          44, 40);
    lv_obj_add_event_cb(p, presetCb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)PRESET_S[i]);
  }

  // P5: tank checkboxes column (rebuilt for the current count at openEditor)
  s_ed_tank_row = lv_obj_create(editors);
  lv_obj_remove_style_all(s_ed_tank_row);
  lv_obj_set_size(s_ed_tank_row, 132, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_ed_tank_row, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_ed_tank_row, 4, 0);

  // v1.9.0 (A1): weekday chips — full width row under the rollers
  lv_obj_t *days_wrap = lv_obj_create(card);
  lv_obj_remove_style_all(days_wrap);
  lv_obj_set_width(days_wrap, lv_pct(100));
  lv_obj_set_height(days_wrap, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(days_wrap, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(days_wrap, 4, 0);
  lv_obj_clear_flag(days_wrap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *ld = lv_label_create(days_wrap);
  lv_label_set_text(ld, "DAYS");
  lv_obj_set_style_text_font(ld, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(ld, lv_color_hex(C_MUTED), 0);
  s_ed_days_row = lv_obj_create(days_wrap);
  lv_obj_remove_style_all(s_ed_days_row);
  lv_obj_set_size(s_ed_days_row, lv_pct(100), 48);
  lv_obj_set_flex_flow(s_ed_days_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(s_ed_days_row, 6, 0);
  lv_obj_set_flex_align(s_ed_days_row, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(s_ed_days_row, LV_OBJ_FLAG_SCROLLABLE);
  static const char *DAY_LETTERS[7] = {"M", "T", "W", "T", "F", "S", "S"};
  for (int i = 0; i < 7; i++) {
    lv_obj_t *chip = makeBtn(s_ed_days_row, DAY_LETTERS[i], C_AMBER, C_INK,
                             C_CARD_HI, 72, 48);
    lv_obj_add_state(chip, LV_STATE_CHECKED); // daily by default
    lv_obj_add_event_cb(chip, dayChipCb, LV_EVENT_CLICKED, nullptr);
    s_ed_day_btn[i] = chip;
  }

  // v1.9.0 (A2): per-tank seconds overrides (visible in multi-tank setups)
  lv_obj_t *sec_wrap = lv_obj_create(card);
  lv_obj_remove_style_all(sec_wrap);
  lv_obj_set_width(sec_wrap, lv_pct(100));
  lv_obj_set_height(sec_wrap, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(sec_wrap, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(sec_wrap, 4, 0);
  lv_obj_clear_flag(sec_wrap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lost = lv_label_create(sec_wrap);
  lv_label_set_text(lost, "PER-TANK SECONDS (0 = same for all)");
  lv_obj_set_style_text_font(lost, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lost, lv_color_hex(C_MUTED), 0);
  s_ed_secs_row = lv_obj_create(sec_wrap);
  lv_obj_remove_style_all(s_ed_secs_row);
  lv_obj_set_size(s_ed_secs_row, lv_pct(100), 64);
  lv_obj_set_flex_flow(s_ed_secs_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(s_ed_secs_row, 10, 0);
  lv_obj_set_flex_align(s_ed_secs_row, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(s_ed_secs_row, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < CATFISH_MAX_TANKS; i++) {
    lv_obj_t *cell = lv_obj_create(s_ed_secs_row);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cell, 2, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lab = lv_label_create(cell);
    lv_label_set_text_fmt(lab, "T%u", (unsigned)(i + 1));
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lab, 110);
    lv_obj_t *sb = makeSpinbox(cell, 0, 30, 0, 110, 48);
    s_ed_sec_sb[i] = sb;
  }

  lv_obj_t *btns = lv_obj_create(card);
  lv_obj_remove_style_all(btns);
  lv_obj_set_width(btns, lv_pct(100));
  lv_obj_set_height(btns, 64);
  lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(btns, 12, 0);
  lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *save = makeBtn(btns, LV_SYMBOL_OK "  Save", C_AMBER, C_INK,
                           C_AMBER_HI, 100, 64);
  lv_obj_set_flex_grow(save, 1);
  lv_obj_add_event_cb(save, editorSaveCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *cancel = makeBtn(btns, LV_SYMBOL_CLOSE "  Cancel", C_CARD, C_MUTED,
                             C_CARD_HI, 100, 64);
  lv_obj_set_flex_grow(cancel, 1);
  lv_obj_add_event_cb(cancel, editorCancelCb, LV_EVENT_CLICKED, nullptr);

  // --- v1.9.0 (C1): history overlay (hidden; content pushed via ui_set_history)
  s_hist_overlay = lv_obj_create(s_scr[1]);
  lv_obj_remove_style_all(s_hist_overlay);
  lv_obj_set_size(s_hist_overlay, lv_pct(100), 360);
  lv_obj_align(s_hist_overlay, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_style_bg_color(s_hist_overlay, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_hist_overlay, LV_OPA_90, 0);
  lv_obj_add_flag(s_hist_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_hist_overlay, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *hcard = makeCard(s_hist_overlay, 736, 336);
  lv_obj_align(hcard, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_set_flex_flow(hcard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(hcard, 8, 0);

  lv_obj_t *htitle = lv_label_create(hcard);
  lv_label_set_text(htitle, "FEED HISTORY (latest 24)");
  lv_obj_set_style_text_font(htitle, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(htitle, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_text_letter_space(htitle, 2, 0);

  s_hist_lbl = lv_label_create(hcard);
  lv_label_set_text(s_hist_lbl, "No feed events yet.");
  lv_obj_set_style_text_font(s_hist_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_hist_lbl, lv_color_hex(C_TEXT), 0);
  lv_label_set_long_mode(s_hist_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_hist_lbl, lv_pct(100));

  lv_obj_t *hclose = makeBtn(hcard, LV_SYMBOL_CLOSE "  Close", C_CARD, C_MUTED,
                             C_CARD_HI, lv_pct(100), 60);
  lv_obj_add_event_cb(hclose, histCloseCb, LV_EVENT_CLICKED, nullptr);
}

void buildSettings() {
  s_scr[2] = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_scr[2], lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_scr[2], LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr[2], LV_OBJ_FLAG_SCROLLABLE);

  addStatusBar(s_scr[2], 2);
  addNavBar(s_scr[2], 2);
  lv_obj_t *c = addContent(s_scr[2]);

  // scrollable stack of setting cards
  lv_obj_t *list = lv_obj_create(c);
  lv_obj_remove_style_all(list);
  lv_obj_set_flex_grow(list, 1);
  lv_obj_set_width(list, lv_pct(100));
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 12, 0);

  // -- Feeding card
  lv_obj_t *card_feed = makeCard(list, lv_pct(100), 100);
  lv_obj_set_flex_flow(card_feed, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(card_feed, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(card_feed, 12, 0);
  lv_obj_t *lbl_p = lv_label_create(card_feed);
  lv_label_set_text(lbl_p, "Default pulse seconds");
  lv_obj_set_style_text_font(lbl_p, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lbl_p, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_grow(lbl_p, 1);
  s_sb_pulse = makeSpinRow(card_feed, 1, 30, prefs::getDefaultPulse(), pulseSpinCb);

  // -- v1.9.0 (A5/F3): Feeding policy card (vacation + guards + catch-up)
  lv_obj_t *card_pol = makeCard(list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_pol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_pol, 10, 0);

  lv_obj_t *row_vac = lv_obj_create(card_pol);
  lv_obj_remove_style_all(row_vac);
  lv_obj_set_width(row_vac, lv_pct(100));
  lv_obj_set_height(row_vac, 44);
  lv_obj_set_flex_flow(row_vac, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_vac, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row_vac, 12, 0);
  lv_obj_clear_flag(row_vac, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lbl_vac = lv_label_create(row_vac);
  lv_label_set_text(lbl_vac, "Vacation (pause feeding)");
  lv_obj_set_style_text_font(lbl_vac, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lbl_vac, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_grow(lbl_vac, 1);
  s_sw_vacation = lv_switch_create(row_vac);
  lv_obj_set_size(s_sw_vacation, 72, 36);
  if (prefs::getVacation()) lv_obj_add_state(s_sw_vacation, LV_STATE_CHECKED);
  lv_obj_add_event_cb(s_sw_vacation, vacationSwitchCb, LV_EVENT_VALUE_CHANGED,
                      nullptr);

  lv_obj_t *hint_pol = lv_label_create(card_pol);
  lv_label_set_text(
      hint_pol,
      "Vacation pauses scheduled/catch-up/remote feeds (on-device button stays live).");
  lv_obj_set_style_text_font(hint_pol, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(hint_pol, lv_color_hex(C_MUTED), 0);

  lv_obj_t *pol_grid = lv_obj_create(card_pol);
  lv_obj_remove_style_all(pol_grid);
  lv_obj_set_width(pol_grid, lv_pct(100));
  lv_obj_set_height(pol_grid, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(pol_grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(pol_grid, 24, 0);
  lv_obj_set_style_pad_row(pol_grid, 8, 0);
  lv_obj_clear_flag(pol_grid, LV_OBJ_FLAG_SCROLLABLE);

  auto makePolicyCol = [&](const char *title, int32_t mn, int32_t mx,
                           int32_t val, lv_obj_t **out) {
    lv_obj_t *col = lv_obj_create(pol_grid);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_MUTED), 0);
    *out = makeSpinRow(col, mn, mx, val, nullptr);
  };
  makePolicyCol("MIN GAP (MIN)", 0, 240, prefs::getMinGapMin(), &s_sb_gap);
  makePolicyCol("MAX FEEDS/DAY", 0, 50, prefs::getMaxFeedsDay(), &s_sb_max);
  makePolicyCol("CATCH-UP (MIN)", 0, 120, prefs::getCatchupWin(), &s_sb_ctch);
  makePolicyCol("LOW FOOD (%)", 5, 90, prefs::getHopperLowPct(), &s_sb_low);

  lv_obj_t *pol_save =
      makeBtn(card_pol, LV_SYMBOL_OK "  Save policy", C_CARD, C_AMBER,
              C_CARD_HI, lv_pct(100), 60);
  lv_obj_set_style_border_color(pol_save, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_border_width(pol_save, 2, 0);
  lv_obj_add_event_cb(pol_save, policySaveCb, LV_EVENT_CLICKED, nullptr);

  // -- v1.9.0 (D1): Food level card (per-tank estimator + refill)
  lv_obj_t *card_food = makeCard(list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_food, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_food, 8, 0);
  makeTitle(card_food, "FOOD LEVEL");
  lv_obj_t *hint_food = lv_label_create(card_food);
  lv_label_set_text(
      hint_food,
      "Estimated from fed seconds. Set \"Hopper size\" per tank (Settings > TANKS edit).");
  lv_obj_set_style_text_font(hint_food, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(hint_food, lv_color_hex(C_MUTED), 0);
  s_food_rows = lv_obj_create(card_food);
  lv_obj_remove_style_all(s_food_rows);
  lv_obj_set_width(s_food_rows, lv_pct(100));
  lv_obj_set_height(s_food_rows, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_food_rows, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_food_rows, 6, 0);
  lv_obj_t *refill_all =
      makeBtn(card_food, LV_SYMBOL_REFRESH "  Refill all", C_CARD, C_AMBER,
              C_CARD_HI, lv_pct(100), 60);
  lv_obj_add_event_cb(refill_all, refillCb, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)0xFF);

  // -- Display card (backlight + clock format rows)
  lv_obj_t *card_disp = makeCard(list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_disp, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_disp, 10, 0);

  lv_obj_t *row_bl = lv_obj_create(card_disp);
  lv_obj_remove_style_all(row_bl);
  lv_obj_set_width(row_bl, lv_pct(100));
  lv_obj_set_height(row_bl, 44);
  lv_obj_set_flex_flow(row_bl, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_bl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row_bl, 12, 0);
  lv_obj_clear_flag(row_bl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lbl_d = lv_label_create(row_bl);
  lv_label_set_text(lbl_d, "Backlight");
  lv_obj_set_style_text_font(lbl_d, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lbl_d, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_grow(lbl_d, 1);
  s_sw_bl = lv_switch_create(row_bl);
  lv_obj_set_size(s_sw_bl, 72, 36);
  if (s_bl_on) lv_obj_add_state(s_sw_bl, LV_STATE_CHECKED);
  lv_obj_add_event_cb(s_sw_bl, blSwitchCb, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *row_h12 = lv_obj_create(card_disp);
  lv_obj_remove_style_all(row_h12);
  lv_obj_set_width(row_h12, lv_pct(100));
  lv_obj_set_height(row_h12, 44);
  lv_obj_set_flex_flow(row_h12, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_h12, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row_h12, 12, 0);
  lv_obj_clear_flag(row_h12, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lbl_h12 = lv_label_create(row_h12);
  lv_label_set_text(lbl_h12, "12-hour clock");
  lv_obj_set_style_text_font(lbl_h12, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lbl_h12, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_grow(lbl_h12, 1);
  s_sw_h12 = lv_switch_create(row_h12);
  lv_obj_set_size(s_sw_h12, 72, 36);
  if (s_hour12) lv_obj_add_state(s_sw_h12, LV_STATE_CHECKED);
  lv_obj_add_event_cb(s_sw_h12, h12SwitchCb, LV_EVENT_VALUE_CHANGED, nullptr);

  // -- Network card
  lv_obj_t *card_net = makeCard(list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_net, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_net, 4, 0);
  makeTitle(card_net, "NETWORK");
  s_lbl_ssid = lv_label_create(card_net);
  lv_label_set_text(s_lbl_ssid, "SSID:  not connected");
  s_lbl_ip = lv_label_create(card_net);
  lv_label_set_text(s_lbl_ip, "IP:  --");
  s_lbl_rssi = lv_label_create(card_net);
  lv_label_set_text(s_lbl_rssi, "RSSI:  --");
  for (lv_obj_t *l : {s_lbl_ssid, s_lbl_ip, s_lbl_rssi}) {
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
  }

  // -- Relay card
  lv_obj_t *card_relay = makeCard(list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_relay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_relay, 8, 0);
  makeTitle(card_relay, "RELAY UNIT");
  s_lbl_relay_ip = lv_label_create(card_relay);
  lv_label_set_text(s_lbl_relay_ip, "Relay IP: not found");
  lv_obj_set_style_text_font(s_lbl_relay_ip, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_relay_ip, lv_color_hex(C_TEXT), 0);

  // -- pairing token row (opens the keyboard overlay on lv_layer_top)
  lv_obj_t *tok_row = lv_obj_create(card_relay);
  lv_obj_remove_style_all(tok_row);
  lv_obj_set_size(tok_row, lv_pct(100), 64);
  lv_obj_set_flex_flow(tok_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tok_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(tok_row, 12, 0);
  lv_obj_clear_flag(tok_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *tok_lbl = lv_label_create(tok_row);
  lv_label_set_text(tok_lbl, "Pairing token");
  lv_obj_set_style_text_font(tok_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(tok_lbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_grow(tok_lbl, 1);
  lv_obj_t *tok_edit = makeBtn(tok_row, LV_SYMBOL_EDIT "  Edit", C_CARD,
                               C_AMBER, C_CARD_HI, 140, 64);
  lv_obj_add_event_cb(tok_edit, tokenEditCb, LV_EVENT_CLICKED, nullptr);

  // -- mechanism row (P4): current selection + Configure button
  lv_obj_t *mech_row = lv_obj_create(card_relay);
  lv_obj_remove_style_all(mech_row);
  lv_obj_set_size(mech_row, lv_pct(100), 64);
  lv_obj_set_flex_flow(mech_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(mech_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(mech_row, 12, 0);
  lv_obj_clear_flag(mech_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *mech_lbl = lv_label_create(mech_row);
  lv_label_set_text(mech_lbl, "Mechanism");
  lv_obj_set_style_text_font(mech_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(mech_lbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_grow(mech_lbl, 1);
  lv_obj_t *mech_edit = makeBtn(mech_row, LV_SYMBOL_SETTINGS "  Configure",
                                C_CARD, C_AMBER, C_CARD_HI, 190, 64);
  lv_obj_add_event_cb(mech_edit, mechEditCb, LV_EVENT_CLICKED, nullptr);
  s_lbl_mech = lv_label_create(card_relay);
  lv_label_set_text(s_lbl_mech, "Mechanism: not reported yet");
  lv_obj_set_style_text_font(s_lbl_mech, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_mech, lv_color_hex(C_MUTED), 0);

  lv_obj_t *test = makeBtn(card_relay, LV_SYMBOL_REFRESH "  Test connection",
                           C_CARD, C_AMBER, C_CARD_HI, lv_pct(100), 64);
  lv_obj_set_style_border_color(test, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_border_width(test, 2, 0);
  lv_obj_set_style_bg_color(test, lv_color_hex(C_CARD), LV_STATE_DISABLED);
  lv_obj_set_style_text_color(test, lv_color_hex(C_MUTED), LV_STATE_DISABLED);
  lv_obj_add_event_cb(test, relayTestCb, LV_EVENT_CLICKED, nullptr);
  s_probe_btn = test;

  // -- TANKS card (P5): count, scan, per-tank rows
  lv_obj_t *card_tanks = makeCard(list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_tanks, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_tanks, 8, 0);
  makeTitle(card_tanks, "TANKS");

  lv_obj_t *cnt_row = lv_obj_create(card_tanks);
  lv_obj_remove_style_all(cnt_row);
  lv_obj_set_size(cnt_row, lv_pct(100), 56);
  lv_obj_set_flex_flow(cnt_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cnt_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(cnt_row, 12, 0);
  lv_obj_clear_flag(cnt_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *cnt_lbl = lv_label_create(cnt_row);
  lv_label_set_text(cnt_lbl, "Relay units (tanks)");
  lv_obj_set_style_text_font(cnt_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(cnt_lbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_grow(cnt_lbl, 1);
  s_tank_sb_count = lv_spinbox_create(cnt_row);
  lv_spinbox_set_range(s_tank_sb_count, 1, CATFISH_MAX_TANKS);
  lv_spinbox_set_digit_format(s_tank_sb_count, 1, 0);
  lv_spinbox_set_value(s_tank_sb_count, 1);
  lv_obj_set_size(s_tank_sb_count, 72, 48);
  lv_obj_set_style_bg_color(s_tank_sb_count, lv_color_hex(C_BG_DARK), 0);
  lv_obj_set_style_bg_opa(s_tank_sb_count, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s_tank_sb_count, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(s_tank_sb_count, &lv_font_montserrat_20, 0);
  lv_obj_set_style_border_color(s_tank_sb_count, lv_color_hex(C_CARD_HI), 0);
  lv_obj_set_style_border_width(s_tank_sb_count, 1, 0);
  lv_obj_set_style_radius(s_tank_sb_count, 8, 0);
  lv_obj_add_event_cb(s_tank_sb_count, tankCountSpinCb, LV_EVENT_VALUE_CHANGED,
                      nullptr);
  lv_obj_t *cnt_minus = makeBtn(cnt_row, LV_SYMBOL_MINUS, C_CARD, C_TEXT,
                                C_CARD_HI, 52, 48);
  lv_obj_add_event_cb(cnt_minus, spinMinusCb, LV_EVENT_CLICKED, s_tank_sb_count);
  lv_obj_t *cnt_plus = makeBtn(cnt_row, LV_SYMBOL_PLUS, C_CARD, C_TEXT,
                               C_CARD_HI, 52, 48);
  lv_obj_add_event_cb(cnt_plus, spinPlusCb, LV_EVENT_CLICKED, s_tank_sb_count);

  s_scan_btn = makeBtn(card_tanks, LV_SYMBOL_WIFI "  Scan for relays", C_CARD,
                       C_AMBER, C_CARD_HI, lv_pct(100), 60);
  lv_obj_set_style_border_color(s_scan_btn, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_border_width(s_scan_btn, 2, 0);
  lv_obj_set_style_bg_color(s_scan_btn, lv_color_hex(C_CARD), LV_STATE_DISABLED);
  lv_obj_set_style_text_color(s_scan_btn, lv_color_hex(C_MUTED), LV_STATE_DISABLED);
  lv_obj_add_event_cb(s_scan_btn, scanTanksCb, LV_EVENT_CLICKED, nullptr);

  s_tank_rows = lv_obj_create(card_tanks);
  lv_obj_remove_style_all(s_tank_rows);
  lv_obj_set_width(s_tank_rows, lv_pct(100));
  lv_obj_set_height(s_tank_rows, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_tank_rows, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_tank_rows, 6, 0);

  // -- STORAGE card (v1.7.0, optional SD feed log)
  lv_obj_t *card_sd = makeCard(list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_sd, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_sd, 8, 0);
  makeTitle(card_sd, "STORAGE");
  s_lbl_sd = lv_label_create(card_sd);
  lv_label_set_text(s_lbl_sd, "SD card: ...");
  lv_obj_set_style_text_font(s_lbl_sd, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_sd, lv_color_hex(C_TEXT), 0);

  // -- RADIOS card (v1.8.0, optional ESP-NOW + nRF24L01 feed nodes)
  lv_obj_t *card_rf = makeCard(list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_rf, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_rf, 8, 0);
  makeTitle(card_rf, "RADIOS");
  s_lbl_espnow = lv_label_create(card_rf);
  lv_label_set_text(s_lbl_espnow, "ESP-NOW: ...");
  lv_obj_set_style_text_font(s_lbl_espnow, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_espnow, lv_color_hex(C_TEXT), 0);
  s_lbl_rf24 = lv_label_create(card_rf);
  lv_label_set_text(s_lbl_rf24, "nRF24L01: ...");
  lv_obj_set_style_text_font(s_lbl_rf24, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_rf24, lv_color_hex(C_TEXT), 0);

  // -- Home Assistant / MQTT card
  lv_obj_t *card_mq = makeCard(list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_mq, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_mq, 8, 0);
  makeTitle(card_mq, "HOME ASSISTANT (MQTT)");
  s_mq_status = lv_label_create(card_mq);
  lv_label_set_text(s_mq_status, "MQTT: disabled");
  lv_obj_set_style_text_font(s_mq_status, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_mq_status, lv_color_hex(C_MUTED), 0);
  s_mq_broker = lv_label_create(card_mq);
  lv_label_set_text(s_mq_broker, "Broker: not set");
  lv_obj_set_style_text_font(s_mq_broker, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_mq_broker, lv_color_hex(C_TEXT), 0);
  lv_obj_t *mq_edit = makeBtn(card_mq, LV_SYMBOL_EDIT "  Configure",
                              C_CARD, C_AMBER, C_CARD_HI, lv_pct(100), 64);
  lv_obj_set_style_border_color(mq_edit, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_border_width(mq_edit, 2, 0);
  lv_obj_add_event_cb(mq_edit, mqttEditCb, LV_EVENT_CLICKED, nullptr);

  // -- System card
  lv_obj_t *card_sys = makeCard(list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_sys, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_sys, 8, 0);
  lv_obj_t *ver = lv_label_create(card_sys);
  lv_label_set_text(ver, "Firmware v" CATFISH_APP_VERSION "  (display-unit)");
  lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(ver, lv_color_hex(C_MUTED), 0);
  s_lbl_rtc = lv_label_create(card_sys);
  lv_label_set_text(s_lbl_rtc, "RTC: checking...");
  lv_obj_set_style_text_font(s_lbl_rtc, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_rtc, lv_color_hex(C_MUTED), 0);
  lv_obj_t *rb = makeBtn(card_sys, LV_SYMBOL_POWER "  Reboot", 0x7E352F,
                         C_TEXT, 0x652A25, lv_pct(100), 64);
  lv_obj_add_event_cb(rb, rebootCb, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// Housekeeping helpers
// ---------------------------------------------------------------------------
void updateNextFeedUi() {
  if (!s_next_valid) return;

  const time_t now = time(nullptr);
  struct tm lt;
  if (!localtime_r(&now, &lt)) return;
  if (lt.tm_year < (2025 - 1900)) return; // no NTP yet

  struct tm tgt = lt;
  tgt.tm_hour = s_next_hour;
  tgt.tm_min = s_next_minute;
  tgt.tm_sec = 0;
  tgt.tm_mday += s_next_days; // v1.9.0 (A1): weekday masks push feeds days out
  const time_t tt = mktime(&tgt);

  long diff = (long)difftime(tt, now);
  if (diff < 0) diff = 0;

  const long days = diff / 86400L;
  const long hrs = (diff % 86400L) / 3600L;
  const long mins = (diff % 3600L) / 60L;

  char sub[28];
  if (days > 0)
    snprintf(sub, sizeof(sub), "in %ld d %ld h", days, hrs);
  else if (hrs > 0)
    snprintf(sub, sizeof(sub), "in %ld h %ld m", hrs, mins);
  else if (mins > 0)
    snprintf(sub, sizeof(sub), "in %ld m", mins);
  else
    snprintf(sub, sizeof(sub), "in <1 m");

  char val[24];
  if (s_next_days >= 2) {
    char hm[12];
    catfishFormatHm(hm, sizeof(hm), s_next_hour, s_next_minute, s_hour12);
    snprintf(val, sizeof(val), "T+%dd %s", (int)s_next_days, hm);
  } else if (s_next_tomorrow) {
    char hm[12];
    catfishFormatHm(hm, sizeof(hm), s_next_hour, s_next_minute, s_hour12);
    snprintf(val, sizeof(val), "Tmrw %s", hm);
  } else {
    catfishFormatHm(val, sizeof(val), s_next_hour, s_next_minute, s_hour12);
  }

  if (strcmp(val, s_val_cache) != 0) {
    strlcpy(s_val_cache, val, sizeof(s_val_cache));
    if (s_next_value) lv_label_set_text(s_next_value, val);
  }
  if (strcmp(sub, s_sub_cache) != 0) {
    strlcpy(s_sub_cache, sub, sizeof(s_sub_cache));
    if (s_next_sub) lv_label_set_text(s_next_sub, sub);
  }
}

void toastTimerInit() {
  s_toast_box = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_toast_box);
  lv_obj_set_size(s_toast_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(s_toast_box, lv_color_hex(C_CARD_HI), 0);
  lv_obj_set_style_bg_opa(s_toast_box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_toast_box, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_border_width(s_toast_box, 2, 0);
  lv_obj_set_style_radius(s_toast_box, 12, 0);
  lv_obj_set_style_pad_hor(s_toast_box, 24, 0);
  lv_obj_set_style_pad_ver(s_toast_box, 14, 0);
  lv_obj_clear_flag(s_toast_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_toast_box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(s_toast_box, LV_ALIGN_TOP_MID, 0, 64);

  s_toast_lbl = lv_label_create(s_toast_box);
  lv_label_set_text(s_toast_lbl, "");
  lv_obj_set_style_text_font(s_toast_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_toast_lbl, lv_color_hex(C_TEXT), 0);

  s_toast_tmr = lv_timer_create(toastTimerCb, 2200, nullptr);
  lv_timer_pause(s_toast_tmr);
}

// ---------------------------------------------------------------------------
// Pairing-token editor (lv_layer_top overlay: card + on-screen keyboard)
// ---------------------------------------------------------------------------
void buildTokenOverlay() {
  s_tok_overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_tok_overlay);
  lv_obj_set_size(s_tok_overlay, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_tok_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_tok_overlay, LV_OPA_70, 0);
  lv_obj_add_flag(s_tok_overlay, LV_OBJ_FLAG_CLICKABLE); // swallow clicks behind
  lv_obj_add_flag(s_tok_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_tok_overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *card = makeCard(s_tok_overlay, 720, 416);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 12, 0);
  lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE); // field scrolls above keyboard

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "PAIRING TOKEN");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_text_letter_space(title, 2, 0);

  lv_obj_t *hint = lv_label_create(card);
  lv_label_set_text(
      hint, "Must match the relay unit token (Settings value it accepts on /feed).");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), 0);

  // M4: optionally rotate the token on the relay unit via POST /config.
  // Only succeeds while the current display token still matches the relay.
  s_tok_rotate = lv_checkbox_create(card);
  lv_checkbox_set_text(s_tok_rotate, "Also update the relay unit");
  lv_obj_set_style_text_font(s_tok_rotate, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_tok_rotate, lv_color_hex(C_TEXT), 0);

  s_tok_ta = lv_textarea_create(card);
  lv_textarea_set_one_line(s_tok_ta, true);
  lv_textarea_set_max_length(s_tok_ta, CATFISH_TOKEN_LEN - 1);
  lv_textarea_set_placeholder_text(s_tok_ta, "relay pairing token");
  lv_obj_set_width(s_tok_ta, lv_pct(100));
  lv_obj_set_style_bg_color(s_tok_ta, lv_color_hex(C_BG_DARK), 0);
  lv_obj_set_style_bg_opa(s_tok_ta, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s_tok_ta, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(s_tok_ta, &lv_font_montserrat_16, 0);
  lv_obj_set_style_border_color(s_tok_ta, lv_color_hex(C_CARD_HI), 0);
  lv_obj_set_style_border_width(s_tok_ta, 1, 0);
  lv_obj_set_style_radius(s_tok_ta, 8, 0);
  lv_obj_set_style_pad_all(s_tok_ta, 10, 0);
  lv_obj_add_event_cb(s_tok_ta, tokenTaFocusCb, LV_EVENT_FOCUSED, nullptr);

  lv_obj_t *btns = lv_obj_create(card);
  lv_obj_remove_style_all(btns);
  lv_obj_set_width(btns, lv_pct(100));
  lv_obj_set_height(btns, 64);
  lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(btns, 12, 0);
  lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *save = makeBtn(btns, LV_SYMBOL_OK "  Save", C_AMBER, C_INK,
                           C_AMBER_HI, 100, 64);
  lv_obj_set_flex_grow(save, 1);
  lv_obj_add_event_cb(save, tokenSaveCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *cancel = makeBtn(btns, LV_SYMBOL_CLOSE "  Cancel", C_CARD, C_MUTED,
                             C_CARD_HI, 100, 64);
  lv_obj_set_flex_grow(cancel, 1);
  lv_obj_add_event_cb(cancel, tokenCancelCb, LV_EVENT_CLICKED, nullptr);

  s_tok_kb = lv_keyboard_create(s_tok_overlay);
  lv_obj_set_size(s_tok_kb, lv_pct(100), 210);
  lv_obj_align(s_tok_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  kbStyle(s_tok_kb);
  lv_obj_add_event_cb(s_tok_kb, kbReadyCb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(s_tok_kb, kbCancelCb, LV_EVENT_CANCEL, nullptr);
  lv_keyboard_set_textarea(s_tok_kb, nullptr);
  lv_obj_add_flag(s_tok_kb, LV_OBJ_FLAG_HIDDEN); // appears on field focus (v1.6.1)
}

void openTokenOverlay() {
  if (!s_tok_overlay) return;
  lv_textarea_set_text(s_tok_ta, prefs::getRelayToken().c_str());
  if (s_tok_rotate) lv_obj_clear_state(s_tok_rotate, LV_STATE_CHECKED);
  lv_obj_add_flag(s_tok_kb, LV_OBJ_FLAG_HIDDEN); // field focus brings it up
  lv_obj_clear_flag(s_tok_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_tok_overlay);
}

void closeTokenOverlay() {
  if (s_tok_overlay) lv_obj_add_flag(s_tok_overlay, LV_OBJ_FLAG_HIDDEN);
  kbHide(s_tok_kb);
}

void tokenTaFocusCb(lv_event_t *e) {
  kbShowFor(s_tok_kb, (lv_obj_t *)lv_event_get_target(e));
}

// ---------------------------------------------------------------------------
// MQTT editor overlay
// ---------------------------------------------------------------------------
void buildMqttOverlay() {
  s_mq_overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_mq_overlay);
  lv_obj_set_size(s_mq_overlay, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_mq_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_mq_overlay, LV_OPA_70, 0);
  lv_obj_add_flag(s_mq_overlay, LV_OBJ_FLAG_CLICKABLE); // swallow clicks behind
  lv_obj_add_flag(s_mq_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_mq_overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *card = makeCard(s_mq_overlay, 720, 416);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 10, 0);
  lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE); // fields scroll above keyboard

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "MQTT BROKER");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_text_letter_space(title, 2, 0);

  lv_obj_t *hint = lv_label_create(card);
  lv_label_set_text(
      hint, "Publishes feeder state to Home Assistant via MQTT auto-discovery.");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), 0);

  // enabled switch row
  lv_obj_t *en_row = lv_obj_create(card);
  lv_obj_remove_style_all(en_row);
  lv_obj_set_width(en_row, lv_pct(100));
  lv_obj_set_height(en_row, 40);
  lv_obj_set_flex_flow(en_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(en_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(en_row, 12, 0);
  lv_obj_clear_flag(en_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *en_lbl = lv_label_create(en_row);
  lv_label_set_text(en_lbl, "Connect to broker");
  lv_obj_set_style_text_font(en_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(en_lbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_grow(en_lbl, 1);
  s_mq_sw_en = lv_switch_create(en_row);
  lv_obj_set_size(s_mq_sw_en, 72, 36);

  // helper lambda-equivalent: one styled one-line textarea
  auto makeTa = [&](lv_obj_t *parent, const char *placeholder, bool secret,
                    uint8_t max_len) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_BG_DARK), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_CARD_HI), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 8, 0);
    lv_obj_set_style_pad_all(ta, 8, 0);
    if (secret) lv_textarea_set_password_mode(ta, true);
    lv_obj_add_event_cb(ta, mqttTaFocusCb, LV_EVENT_FOCUSED, nullptr);
    return ta;
  };

  s_mq_ta_host = makeTa(card, "broker host or IP", false,
                        CATFISH_MQTT_HOST_LEN - 1);

  // port + prefix row
  lv_obj_t *pp_row = lv_obj_create(card);
  lv_obj_remove_style_all(pp_row);
  lv_obj_set_width(pp_row, lv_pct(100));
  lv_obj_set_height(pp_row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(pp_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pp_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(pp_row, 12, 0);
  lv_obj_clear_flag(pp_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *port_lbl = lv_label_create(pp_row);
  lv_label_set_text(port_lbl, "Port");
  lv_obj_set_style_text_font(port_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(port_lbl, lv_color_hex(C_TEXT), 0);
  s_mq_sb_port = lv_spinbox_create(pp_row);
  lv_spinbox_set_range(s_mq_sb_port, 1, 65535);
  lv_spinbox_set_digit_format(s_mq_sb_port, 5, 0);
  lv_spinbox_set_value(s_mq_sb_port, CATFISH_MQTT_PORT);
  lv_obj_set_size(s_mq_sb_port, 110, 44);
  lv_obj_set_style_bg_color(s_mq_sb_port, lv_color_hex(C_BG_DARK), 0);
  lv_obj_set_style_bg_opa(s_mq_sb_port, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s_mq_sb_port, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(s_mq_sb_port, &lv_font_montserrat_16, 0);
  lv_obj_set_style_border_color(s_mq_sb_port, lv_color_hex(C_CARD_HI), 0);
  lv_obj_set_style_border_width(s_mq_sb_port, 1, 0);
  lv_obj_set_style_radius(s_mq_sb_port, 8, 0);
  lv_obj_t *pre_lbl = lv_label_create(pp_row);
  lv_label_set_text(pre_lbl, "Prefix");
  lv_obj_set_style_text_font(pre_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(pre_lbl, lv_color_hex(C_TEXT), 0);
  s_mq_ta_prefix = lv_textarea_create(pp_row);
  lv_textarea_set_one_line(s_mq_ta_prefix, true);
  lv_textarea_set_max_length(s_mq_ta_prefix, CATFISH_MQTT_PREFIX_LEN - 1);
  lv_textarea_set_placeholder_text(s_mq_ta_prefix, CATFISH_MQTT_PREFIX);
  lv_obj_set_flex_grow(s_mq_ta_prefix, 1);
  lv_obj_set_style_bg_color(s_mq_ta_prefix, lv_color_hex(C_BG_DARK), 0);
  lv_obj_set_style_bg_opa(s_mq_ta_prefix, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s_mq_ta_prefix, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(s_mq_ta_prefix, &lv_font_montserrat_16, 0);
  lv_obj_set_style_border_color(s_mq_ta_prefix, lv_color_hex(C_CARD_HI), 0);
  lv_obj_set_style_border_width(s_mq_ta_prefix, 1, 0);
  lv_obj_set_style_radius(s_mq_ta_prefix, 8, 0);
  lv_obj_set_style_pad_all(s_mq_ta_prefix, 8, 0);
  lv_obj_add_event_cb(s_mq_ta_prefix, mqttTaFocusCb, LV_EVENT_FOCUSED, nullptr);

  s_mq_ta_user = makeTa(card, "username (optional)", false,
                        CATFISH_MQTT_USER_LEN - 1);
  s_mq_ta_pass = makeTa(card, "password (optional)", true,
                        CATFISH_MQTT_PASS_LEN - 1);

  lv_obj_t *btns = lv_obj_create(card);
  lv_obj_remove_style_all(btns);
  lv_obj_set_width(btns, lv_pct(100));
  lv_obj_set_height(btns, 64);
  lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(btns, 12, 0);
  lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *save = makeBtn(btns, LV_SYMBOL_OK "  Save", C_AMBER, C_INK,
                           C_AMBER_HI, 100, 64);
  lv_obj_set_flex_grow(save, 1);
  lv_obj_add_event_cb(save, mqttSaveCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *cancel = makeBtn(btns, LV_SYMBOL_CLOSE "  Cancel", C_CARD, C_MUTED,
                             C_CARD_HI, 100, 64);
  lv_obj_set_flex_grow(cancel, 1);
  lv_obj_add_event_cb(cancel, mqttCancelCb, LV_EVENT_CLICKED, nullptr);

  s_mq_kb = lv_keyboard_create(s_mq_overlay);
  lv_obj_set_size(s_mq_kb, lv_pct(100), 180);
  lv_obj_align(s_mq_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  kbStyle(s_mq_kb);
  lv_obj_add_event_cb(s_mq_kb, kbReadyCb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(s_mq_kb, kbCancelCb, LV_EVENT_CANCEL, nullptr);
  lv_keyboard_set_textarea(s_mq_kb, nullptr);
  lv_obj_add_flag(s_mq_kb, LV_OBJ_FLAG_HIDDEN); // appears on first field focus
}

void openMqttOverlay() {
  if (!s_mq_overlay) return;
  lv_textarea_set_text(s_mq_ta_host, prefs::getMqttHost().c_str());
  spinSetValueQuiet(s_mq_sb_port, prefs::getMqttPort());
  lv_textarea_set_text(s_mq_ta_prefix, prefs::getMqttPrefix().c_str());
  lv_textarea_set_text(s_mq_ta_user, prefs::getMqttUser().c_str());
  lv_textarea_set_text(s_mq_ta_pass, prefs::getMqttPass().c_str());
  if (prefs::getMqttEnabled())
    lv_obj_add_state(s_mq_sw_en, LV_STATE_CHECKED);
  else
    lv_obj_clear_state(s_mq_sw_en, LV_STATE_CHECKED);
  lv_obj_add_flag(s_mq_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_mq_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_mq_overlay);
}

void closeMqttOverlay() {
  if (s_mq_overlay) lv_obj_add_flag(s_mq_overlay, LV_OBJ_FLAG_HIDDEN);
  kbHide(s_mq_kb);
}

// ---------------------------------------------------------------------------
// Mechanism editor overlay (P4)
// ---------------------------------------------------------------------------
void buildMechOverlay() {
  s_mech_overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_mech_overlay);
  lv_obj_set_size(s_mech_overlay, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_mech_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_mech_overlay, LV_OPA_70, 0);
  lv_obj_add_flag(s_mech_overlay, LV_OBJ_FLAG_CLICKABLE); // swallow clicks behind
  lv_obj_add_flag(s_mech_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_mech_overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *card = makeCard(s_mech_overlay, 720, 430);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 44);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 10, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "FEEDING MECHANISM");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_text_letter_space(title, 2, 0);

  lv_obj_t *hint = lv_label_create(card);
  lv_label_set_text(
      hint,
      "All mechanisms feed for the configured seconds. Wiring: servo D5, "
      "stepper STEP D2 / DIR D6 (see docs/firmware.md).");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), 0);
  lv_obj_set_width(hint, lv_pct(100));
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

  s_mech_roller = makeRoller(card, "Motor relay\nServo\nStepper", 0);
  lv_obj_add_event_cb(s_mech_roller, mechRollerCb, LV_EVENT_VALUE_CHANGED, nullptr);

  // -- servo params row (hidden unless Servo selected)
  s_mech_row_servo = lv_obj_create(card);
  lv_obj_remove_style_all(s_mech_row_servo);
  lv_obj_set_width(s_mech_row_servo, lv_pct(100));
  lv_obj_set_height(s_mech_row_servo, 64);
  lv_obj_set_flex_flow(s_mech_row_servo, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_mech_row_servo, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s_mech_row_servo, 10, 0);
  lv_obj_clear_flag(s_mech_row_servo, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *rest_lbl = lv_label_create(s_mech_row_servo);
  lv_label_set_text(rest_lbl, "Rest\xC2\xB0");
  lv_obj_set_style_text_font(rest_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(rest_lbl, lv_color_hex(C_TEXT), 0);
  s_mech_sb_rest = makeSpinbox(s_mech_row_servo, CATFISH_SERVO_DEG_MIN,
                               CATFISH_SERVO_DEG_MAX, CATFISH_SERVO_REST_DEFAULT,
                               96, 56);
  lv_spinbox_set_digit_format(s_mech_sb_rest, 3, 0);
  lv_obj_t *feed_lbl = lv_label_create(s_mech_row_servo);
  lv_label_set_text(feed_lbl, "Feed\xC2\xB0");
  lv_obj_set_style_text_font(feed_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(feed_lbl, lv_color_hex(C_TEXT), 0);
  s_mech_sb_feed = makeSpinbox(s_mech_row_servo, CATFISH_SERVO_DEG_MIN,
                               CATFISH_SERVO_DEG_MAX, CATFISH_SERVO_FEED_DEFAULT,
                               96, 56);
  lv_spinbox_set_digit_format(s_mech_sb_feed, 3, 0);

  // -- stepper params row (hidden unless Stepper selected)
  s_mech_row_step = lv_obj_create(card);
  lv_obj_remove_style_all(s_mech_row_step);
  lv_obj_set_width(s_mech_row_step, lv_pct(100));
  lv_obj_set_height(s_mech_row_step, 64);
  lv_obj_set_flex_flow(s_mech_row_step, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_mech_row_step, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s_mech_row_step, 10, 0);
  lv_obj_clear_flag(s_mech_row_step, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *hz_lbl = lv_label_create(s_mech_row_step);
  lv_label_set_text(hz_lbl, "Steps/s");
  lv_obj_set_style_text_font(hz_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hz_lbl, lv_color_hex(C_TEXT), 0);
  s_mech_sb_hz = makeSpinbox(s_mech_row_step, CATFISH_STEP_HZ_MIN,
                             CATFISH_STEP_HZ_MAX, CATFISH_STEP_HZ_DEFAULT, 110, 56);
  lv_spinbox_set_digit_format(s_mech_sb_hz, 4, 0);
  lv_obj_t *dir_lbl = lv_label_create(s_mech_row_step);
  lv_label_set_text(dir_lbl, "DIR high");
  lv_obj_set_style_text_font(dir_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(dir_lbl, lv_color_hex(C_TEXT), 0);
  s_mech_sw_dir = lv_switch_create(s_mech_row_step);
  lv_obj_set_size(s_mech_sw_dir, 72, 36);

  lv_obj_t *btns = lv_obj_create(card);
  lv_obj_remove_style_all(btns);
  lv_obj_set_width(btns, lv_pct(100));
  lv_obj_set_height(btns, 64);
  lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(btns, 12, 0);
  lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *save = makeBtn(btns, LV_SYMBOL_OK "  Save", C_AMBER, C_INK,
                           C_AMBER_HI, 100, 64);
  lv_obj_set_flex_grow(save, 1);
  lv_obj_add_event_cb(save, mechSaveCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *cancel = makeBtn(btns, LV_SYMBOL_CLOSE "  Cancel", C_CARD, C_MUTED,
                             C_CARD_HI, 100, 64);
  lv_obj_set_flex_grow(cancel, 1);
  lv_obj_add_event_cb(cancel, mechCancelCb, LV_EVENT_CLICKED, nullptr);

  mechSyncRows();
}

void openMechOverlay() {
  if (!s_mech_overlay || !s_mech_roller) return;
  // prefill from the last /status probe (falls back to shared defaults)
  const uint8_t id = s_mech_reported ? s_mech_id : CATFISH_MECH_DEFAULT;
  lv_roller_set_selected(s_mech_roller, id, LV_ANIM_OFF);
  spinSetValueQuiet(s_mech_sb_rest,
                    (s_mech_rest >= 0) ? s_mech_rest : CATFISH_SERVO_REST_DEFAULT);
  spinSetValueQuiet(s_mech_sb_feed,
                    (s_mech_feed >= 0) ? s_mech_feed : CATFISH_SERVO_FEED_DEFAULT);
  spinSetValueQuiet(s_mech_sb_hz,
                    (s_mech_hz >= 0) ? s_mech_hz : CATFISH_STEP_HZ_DEFAULT);
  lv_obj_clear_state(s_mech_sw_dir, LV_STATE_CHECKED); // DIR level not reported; default low
  mechSyncRows();
  lv_obj_clear_flag(s_mech_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_mech_overlay);
}

void closeMechOverlay() {
  if (s_mech_overlay) lv_obj_add_flag(s_mech_overlay, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Multi-tank widgets (P5)
// ---------------------------------------------------------------------------
void tankChipCb(lv_event_t *e) {
  // user_data: 0-based tank index, or 0xFE for the "All" chip
  UiEvent ev;
  ev.type = UI_EVT_SELECT_TANK;
  ev.a = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  uiEventSend(ev);
}

void tankCountSpinCb(lv_event_t *e) {
  (void)e;
  if (!s_tank_sb_count) return;
  if (s_spin_prog) return; // programmatic ui_tanks_rebuild() replay (see guard note)
  UiEvent ev;
  ev.type = UI_EVT_TANK_COUNT;
  ev.a = (uint8_t)lv_spinbox_get_value(s_tank_sb_count);
  uiEventSend(ev); // loop persists + rebuilds everything
}

void scanTanksCb(lv_event_t *e) {
  (void)e;
  UiEvent ev;
  ev.type = UI_EVT_SCAN_TANKS;
  uiEventSend(ev); // loop disables the button via ui_set_scan_busy(true)
}

void tankEditCb(lv_event_t *e) {
  openTankOverlay((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

void tankCancelCb(lv_event_t *e) {
  (void)e;
  closeTankOverlay();
}

void tankTaFocusCb(lv_event_t *e) {
  kbShowFor(s_tank_kb, (lv_obj_t *)lv_event_get_target(e));
}

void tankSaveCb(lv_event_t *e) {
  (void)e;
  const char *name = lv_textarea_get_text(s_tank_ta_name);
  if (!name || name[0] == '\0') {
    ui_toast("Tank name required");
    return;
  }
  UiEvent ev;
  ev.type = UI_EVT_TANK_CFG;
  ev.a = s_tank_edit_id; // 1-based tank id
  strlcpy(ev.tankName, name, sizeof(ev.tankName));
  strlcpy(ev.token, lv_textarea_get_text(s_tank_ta_tok), sizeof(ev.token));
  // v1.9.0 (A3/D1): per-tank food rate + hopper capacity
  if (s_tank_sb_gps) ev.tankGps = (uint16_t)lv_spinbox_get_value(s_tank_sb_gps);
  if (s_tank_sb_hcap) ev.tankHcap = (uint16_t)lv_spinbox_get_value(s_tank_sb_hcap);
  if (uiEventSend(ev)) closeTankOverlay(); // loop persists + rebuilds
}

void buildTankOverlay() {
  s_tank_overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_tank_overlay);
  lv_obj_set_size(s_tank_overlay, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_tank_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_tank_overlay, LV_OPA_70, 0);
  lv_obj_add_flag(s_tank_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_tank_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_tank_overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *card = makeCard(s_tank_overlay, 720, 416);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 12, 0);
  lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE); // fields scroll above keyboard

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "EDIT TANK");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(C_AMBER), 0);
  lv_obj_set_style_text_letter_space(title, 2, 0);

  lv_obj_t *hint = lv_label_create(card);
  lv_label_set_text(
      hint, "Token override: leave empty to use the shared pairing token.");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), 0);

  auto makeTa = [&](lv_obj_t *parent, const char *placeholder, bool secret,
                    uint8_t max_len) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_BG_DARK), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_CARD_HI), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 8, 0);
    lv_obj_set_style_pad_all(ta, 10, 0);
    if (secret) lv_textarea_set_password_mode(ta, true);
    lv_obj_add_event_cb(ta, tankTaFocusCb, LV_EVENT_FOCUSED, nullptr);
    return ta;
  };
  s_tank_ta_name = makeTa(card, "tank name (e.g. Living room)", false,
                          CATFISH_TANK_NAME_LEN - 1);
  s_tank_ta_tok = makeTa(card, "token override (optional)", true,
                         CATFISH_TOKEN_LEN - 1);

  // v1.9.0 (A3/D1): food rate + hopper size (drive grams display + estimator)
  lv_obj_t *num_row = lv_obj_create(card);
  lv_obj_remove_style_all(num_row);
  lv_obj_set_width(num_row, lv_pct(100));
  lv_obj_set_height(num_row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(num_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(num_row, 24, 0);
  lv_obj_clear_flag(num_row, LV_OBJ_FLAG_SCROLLABLE);
  {
    lv_obj_t *col = lv_obj_create(num_row);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, "FOOD RATE (G/S, 0 = OFF)");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_MUTED), 0);
    s_tank_sb_gps = makeSpinRow(col, 0, 100, 0, nullptr);
  }
  {
    lv_obj_t *col = lv_obj_create(num_row);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, "HOPPER SIZE (SECONDS OF FEED)");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_MUTED), 0);
    // 5-digit spinbox: up to 60000 s (~16 h of pulse) is far beyond any hopper
    s_tank_sb_hcap = makeSpinRow(col, 0, 60000, 0, nullptr);
    lv_spinbox_set_digit_format(s_tank_sb_hcap, 5, 0);
  }

  lv_obj_t *btns = lv_obj_create(card);
  lv_obj_remove_style_all(btns);
  lv_obj_set_width(btns, lv_pct(100));
  lv_obj_set_height(btns, 64);
  lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(btns, 12, 0);
  lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *save = makeBtn(btns, LV_SYMBOL_OK "  Save", C_AMBER, C_INK,
                           C_AMBER_HI, 100, 64);
  lv_obj_set_flex_grow(save, 1);
  lv_obj_add_event_cb(save, tankSaveCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *cancel = makeBtn(btns, LV_SYMBOL_CLOSE "  Cancel", C_CARD, C_MUTED,
                             C_CARD_HI, 100, 64);
  lv_obj_set_flex_grow(cancel, 1);
  lv_obj_add_event_cb(cancel, tankCancelCb, LV_EVENT_CLICKED, nullptr);

  s_tank_kb = lv_keyboard_create(s_tank_overlay);
  lv_obj_set_size(s_tank_kb, lv_pct(100), 210);
  lv_obj_align(s_tank_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  kbStyle(s_tank_kb);
  lv_obj_add_event_cb(s_tank_kb, kbReadyCb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(s_tank_kb, kbCancelCb, LV_EVENT_CANCEL, nullptr);
  lv_keyboard_set_textarea(s_tank_kb, nullptr);
  lv_obj_add_flag(s_tank_kb, LV_OBJ_FLAG_HIDDEN); // appears on field focus
}

void openTankOverlay(uint8_t tank1) {
  if (!s_tank_overlay) return;
  if (tank1 < 1 || tank1 > CATFISH_MAX_TANKS) return;
  s_tank_edit_id = tank1;
  lv_textarea_set_text(s_tank_ta_name, s_tank_names[tank1 - 1]);
  lv_textarea_set_text(s_tank_ta_tok, prefs::getTankToken(tank1).c_str());
  // v1.9.0 (A3/D1): prefill food rate + hopper capacity
  if (s_tank_sb_gps) spinSetValueQuiet(s_tank_sb_gps, prefs::getTankGps(tank1));
  if (s_tank_sb_hcap)
    spinSetValueQuiet(s_tank_sb_hcap, prefs::getTankHopperCap(tank1));
  lv_obj_add_flag(s_tank_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_tank_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_tank_overlay);
}

void closeTankOverlay() {
  if (s_tank_overlay) lv_obj_add_flag(s_tank_overlay, LV_OBJ_FLAG_HIDDEN);
  kbHide(s_tank_kb);
}

void rebuildTankChips() {
  if (!s_tank_chips) return;
  lv_obj_clean(s_tank_chips);
  for (int i = 0; i <= CATFISH_MAX_TANKS; i++) s_tank_chip_btns[i] = nullptr;

  if (s_tank_count <= 1) {
    lv_obj_add_flag(s_tank_chips, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(s_tank_chips, LV_OBJ_FLAG_HIDDEN);

  for (uint8_t i = 0; i < s_tank_count && i < CATFISH_MAX_TANKS; i++) {
    lv_obj_t *chip = makeBtn(s_tank_chips, s_tank_names[i], C_CARD, C_TEXT,
                             C_CARD_HI, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_pad_hor(chip, 14, 0);
    lv_obj_add_event_cb(chip, tankChipCb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)i);
    s_tank_chip_btns[i] = chip;
  }
  // "All" chip: feeds every configured tank (user_data 0xFE)
  lv_obj_t *all = makeBtn(s_tank_chips, "All", C_CARD, C_TEXT, C_CARD_HI,
                          LV_SIZE_CONTENT, 52);
  lv_obj_set_style_pad_hor(all, 14, 0);
  lv_obj_add_event_cb(all, tankChipCb, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)0xFE);
  s_tank_chip_btns[CATFISH_MAX_TANKS] = all;

  // selection highlight
  for (uint8_t i = 0; i < s_tank_count && i < CATFISH_MAX_TANKS; i++) {
    const bool sel = (s_sel_tank == i);
    lv_obj_set_style_bg_color(s_tank_chip_btns[i],
                              lv_color_hex(sel ? C_AMBER : C_CARD), 0);
    lv_obj_set_style_text_color(s_tank_chip_btns[i],
                                lv_color_hex(sel ? C_INK : C_TEXT), 0);
  }
  if (s_tank_chip_btns[CATFISH_MAX_TANKS]) {
    const bool sel = (s_sel_tank == 0xFE);
    lv_obj_set_style_bg_color(s_tank_chip_btns[CATFISH_MAX_TANKS],
                              lv_color_hex(sel ? C_AMBER : C_CARD), 0);
    lv_obj_set_style_text_color(s_tank_chip_btns[CATFISH_MAX_TANKS],
                                lv_color_hex(sel ? C_INK : C_TEXT), 0);
  }
}

void rebuildTankRows() {
  if (!s_tank_rows) return;
  lv_obj_clean(s_tank_rows);
  for (int i = 0; i < CATFISH_MAX_TANKS; i++) {
    s_tank_row_name[i] = nullptr;
    s_tank_row_ip[i] = nullptr;
    s_tank_row_dot[i] = nullptr;
  }

  for (uint8_t i = 0; i < s_tank_count && i < CATFISH_MAX_TANKS; i++) {
    lv_obj_t *row = lv_obj_create(s_tank_rows);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 56);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    s_tank_row_dot[i] = dot;

    char head[40];
    snprintf(head, sizeof(head), "%u \xE2\x80\xA2 %s", (unsigned)(i + 1),
             s_tank_names[i]);
    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, head);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
    s_tank_row_name[i] = name;

    lv_obj_t *ipLbl = lv_label_create(row);
    lv_label_set_text(ipLbl, "--"); // "—" renders as tofu in Montserrat built-ins
    lv_obj_set_style_text_font(ipLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ipLbl, lv_color_hex(C_MUTED), 0);
    lv_obj_set_flex_grow(ipLbl, 1);
    lv_label_set_long_mode(ipLbl, LV_LABEL_LONG_DOT);
    s_tank_row_ip[i] = ipLbl;

    lv_obj_t *edit = makeBtn(row, LV_SYMBOL_EDIT, C_CARD, C_AMBER, C_CARD_HI,
                             56, 56);
    lv_obj_add_event_cb(edit, tankEditCb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(i + 1));
  }
}

// ---------------------------------------------------------------------------
// v1.9.0 (D1): food-level rows (Settings > FOOD) + Home relay-card bar
// ---------------------------------------------------------------------------
void rebuildFoodRows() {
  if (!s_food_rows) return;
  lv_obj_clean(s_food_rows);
  s_food_rows_count = s_tank_count;
  for (uint8_t i = 0; i < s_tank_count && i < CATFISH_MAX_TANKS; i++) {
    lv_obj_t *row = lv_obj_create(s_food_rows);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 52);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, s_tank_names[i]);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(name, 110);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_size(bar, lv_pct(45), 14);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BG_DARK), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_AMBER), LV_PART_INDICATOR);
    lv_obj_add_state(bar, LV_STATE_DISABLED); // styled below via custom color

    lv_obj_t *pct = lv_label_create(row);
    lv_label_set_text(pct, "--");
    lv_obj_set_style_text_font(pct, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pct, lv_color_hex(C_MUTED), 0);

    lv_obj_t *used = lv_label_create(row);
    lv_label_set_text(used, "");
    lv_obj_set_style_text_font(used, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(used, lv_color_hex(C_MUTED), 0);
    lv_obj_set_flex_grow(used, 1);

    lv_obj_t *ref = makeBtn(row, LV_SYMBOL_REFRESH, C_CARD, C_AMBER,
                            C_CARD_HI, 48, 48);
    lv_obj_add_event_cb(ref, refillCb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(i + 1));

    // stash per-row widget refs (data pointers via lv_obj_set_user_data)
    lv_obj_set_user_data(bar, pct);
    lv_obj_set_user_data(pct, used);
  }
  refreshFoodRows();
}

void refreshFoodRows() {
  if (!s_food_rows || lv_obj_get_child_cnt(s_food_rows) == 0) return;
  bool anyKnown = false;
  for (uint8_t i = 0; i < s_food_rows_count && i < CATFISH_MAX_TANKS; i++) {
    const int idx = i;
    lv_obj_t *bar = lv_obj_get_child(s_food_rows, idx);
    if (!bar) continue;
    lv_obj_t *pct = (lv_obj_t *)lv_obj_get_user_data(bar);
    lv_obj_t *used = pct ? (lv_obj_t *)lv_obj_get_user_data(pct) : nullptr;
    const uint8_t p = s_food_pct[i];
    char buf[40];
    if (p == 255) {
      if (bar) {
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_BG_DARK),
                                  LV_PART_INDICATOR);
      }
      if (pct) lv_label_set_text(pct, "set size");
      if (used) lv_label_set_text(used, "");
    } else {
      anyKnown = true;
      if (bar) {
        lv_bar_set_value(bar, p, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar,
                                  lv_color_hex(p < s_food_low ? C_RED : C_AMBER),
                                  LV_PART_INDICATOR);
      }
      if (pct) {
        snprintf(buf, sizeof(buf), "%u%%%s", (unsigned)p,
                 (p < s_food_low) ? " LOW" : "");
        lv_obj_set_style_text_color(
            pct, lv_color_hex(p < s_food_low ? C_RED : C_TEXT), 0);
        lv_label_set_text(pct, buf);
      }
      if (used) {
        if (s_food_gps[i] > 0 && s_food_cap[i] > s_food_used[i]) {
          const uint32_t g = ((uint32_t)(s_food_cap[i] - s_food_used[i]) *
                              s_food_gps[i]);
          snprintf(buf, sizeof(buf), "~%lu g left", (unsigned long)g);
        } else if (s_food_used[i] > 0) {
          snprintf(buf, sizeof(buf), "%lu s fed", (unsigned long)s_food_used[i]);
        } else {
          buf[0] = '\0';
        }
        lv_label_set_text(used, buf);
      }
    }
  }
  // Home relay-card bar: selected tank, or the lowest known configured tank
  if (s_food_home_bar && s_food_home_lbl) {
    if (!anyKnown) {
      lv_obj_add_flag(s_food_home_bar, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_food_home_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(s_food_home_bar, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_food_home_lbl, LV_OBJ_FLAG_HIDDEN);
      uint8_t p = 255;
      if (s_sel_tank != 0xFE && s_sel_tank < CATFISH_MAX_TANKS) {
        p = s_food_pct[s_sel_tank];
      } else {
        for (uint8_t i = 0; i < s_food_rows_count && i < CATFISH_MAX_TANKS;
             i++) {
          if (s_food_pct[i] != 255 && (p == 255 || s_food_pct[i] < p))
            p = s_food_pct[i];
        }
      }
      if (p == 255) p = 0;
      lv_bar_set_value(s_food_home_bar, p, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(
          s_food_home_bar, lv_color_hex(p < s_food_low ? C_RED : C_AMBER),
          LV_PART_INDICATOR);
      char buf[24];
      snprintf(buf, sizeof(buf), "Food: %u%%", (unsigned)p);
      lv_label_set_text(s_food_home_lbl, buf);
    }
  }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API (caller holds lvgl_port_lock unless running inside LVGL task)
// ---------------------------------------------------------------------------
void ui_init() {
  if (!s_evt_queue) s_evt_queue = xQueueCreate(8, sizeof(UiEvent));
  buildRollerOptions();
  buildHome();
  buildSchedule();
  buildSettings();
  toastTimerInit();
  buildTokenOverlay();
  buildMqttOverlay();
  buildMechOverlay();
  buildTankOverlay();

  // default tank state until main() calls ui_tanks_rebuild() with prefs data
  String defNames[CATFISH_MAX_TANKS] = {"Main", "Tank 2", "Tank 3", "Tank 4"};
  ui_tanks_rebuild(1, defNames, 0);

  showScreen(0);
  ui_set_next_feed("Waiting for time sync...");
  ui_set_last_feed("Never");
  Serial.println("[UI] screens built (home, schedule, settings)");
}

bool ui_event_receive(UiEvent &out) {
  if (!s_evt_queue) return false;
  return xQueueReceive(s_evt_queue, &out, 0) == pdTRUE;
}

bool ui_event_send(const UiEvent &ev) { return uiEventSend(ev); }

void ui_tick() {
  // backlight idle auto-dim (M6)
  const uint32_t inactive = lv_disp_get_inactive_time(nullptr);
  if (s_bl_on && !s_dimmed && inactive > UI_BACKLIGHT_TIMEOUT_MS) {
    s_dimmed = true;
    applyBacklight();
    Serial.println("[UI] backlight auto-dim (idle)");
  } else if (s_dimmed && inactive <= 1000) {
    // fresh touch activity -> wake
    s_dimmed = false;
    applyBacklight();
    Serial.println("[UI] backlight wake (touch)");
  }

  // clock (all status bars) — 12/24h per the user preference (v1.6.0)
  const time_t now = time(nullptr);
  struct tm lt;
  char hhmm[12];
  if (localtime_r(&now, &lt) && lt.tm_year >= (2025 - 1900))
    catfishFormatHm(hhmm, sizeof(hhmm), lt.tm_hour, lt.tm_min, s_hour12);
  else
    snprintf(hhmm, sizeof(hhmm), "--:--");

  if (strcmp(hhmm, s_clock_cache) != 0) {
    strlcpy(s_clock_cache, hhmm, sizeof(s_clock_cache));
    for (int i = 0; i < 3; i++) {
      if (s_clock_lbl[i]) lv_label_set_text(s_clock_lbl[i], hhmm);
    }
  }

  // next-feed countdown
  updateNextFeedUi();

  // relay dot pulse while relay not confirmed online and not feeding
  if (s_relay_dot) {
    if (!s_relay_online && !s_relay_feeding) {
      s_dot_blink = !s_dot_blink;
      lv_obj_set_style_bg_opa(s_relay_dot, s_dot_blink ? LV_OPA_COVER : LV_OPA_30, 0);
    } else {
      lv_obj_set_style_bg_opa(s_relay_dot, LV_OPA_COVER, 0);
    }
  }
}

void ui_set_wifi(bool connected, const String &ssid, const String &ip, int rssi) {
  for (int i = 0; i < 3; i++) {
    if (s_wifi_sym[i]) {
      lv_obj_set_style_text_color(s_wifi_sym[i],
                                  lv_color_hex(connected ? C_TEXT : C_MUTED), 0);
    }
  }
  if (s_lbl_ssid) {
    if (connected && ssid.length())
      lv_label_set_text_fmt(s_lbl_ssid, "SSID:  %s", ssid.c_str());
    else
      lv_label_set_text(s_lbl_ssid, "SSID:  not connected");
  }
  if (s_lbl_ip) {
    if (connected && ip.length())
      lv_label_set_text_fmt(s_lbl_ip, "IP:  %s", ip.c_str());
    else
      lv_label_set_text(s_lbl_ip, "IP:  --");
  }
  if (s_lbl_rssi) {
    if (connected)
      lv_label_set_text_fmt(s_lbl_rssi, "RSSI:  %d dBm", rssi);
    else
      lv_label_set_text(s_lbl_rssi, "RSSI:  --");
  }
}

void ui_set_relay(bool online, const String &ip, bool feeding) {
  s_relay_online = online;
  s_relay_feeding = feeding;
  strlcpy(s_relay_ip_cache, ip.c_str(), sizeof(s_relay_ip_cache));

  if (s_relay_dot) {
    lv_obj_set_style_bg_color(s_relay_dot,
                              lv_color_hex(online ? C_GREEN : C_AMBER), 0);
    lv_obj_set_style_bg_opa(s_relay_dot, LV_OPA_COVER, 0);
  }
  if (s_relay_txt) {
    if (s_relay_ip_cache[0]) {
      if (feeding)
        lv_label_set_text_fmt(s_relay_txt, "Feeding...  \xE2\x80\xA2  %s",
                              s_relay_ip_cache);
      else if (online)
        lv_label_set_text_fmt(s_relay_txt, "Online  \xE2\x80\xA2  %s",
                              s_relay_ip_cache);
      else
        lv_label_set_text_fmt(s_relay_txt, "Offline  \xE2\x80\xA2  %s",
                              s_relay_ip_cache);
    } else {
      lv_label_set_text(s_relay_txt, "Searching...");
    }
  }
  if (s_lbl_relay_ip) {
    lv_label_set_text_fmt(s_lbl_relay_ip, "Relay IP: %s",
                          s_relay_ip_cache[0] ? s_relay_ip_cache : "not found");
  }
}

void ui_set_next_feed(const String &text) {
  s_next_valid = false;
  s_val_cache[0] = '\0';
  s_sub_cache[0] = '\0';
  if (s_next_value) lv_label_set_text(s_next_value, text.c_str());
  if (s_next_sub) lv_label_set_text(s_next_sub, "");
}

void ui_set_next_feed_time(int hour, int minute, uint8_t daysAhead, bool valid) {
  s_next_valid = valid;
  s_next_tomorrow = (daysAhead > 0);
  s_next_days = daysAhead;
  s_next_hour = hour;
  s_next_minute = minute;
  s_val_cache[0] = '\0';
  s_sub_cache[0] = '\0';
  if (!valid) {
    if (s_next_value) lv_label_set_text(s_next_value, "--:--");
    if (s_next_sub) lv_label_set_text(s_next_sub, "");
  }
}

void ui_set_last_feed(const String &text) {
  if (s_last_value) lv_label_set_text(s_last_value, text.c_str());
}

void ui_toast(const String &msg) {
  if (!s_toast_box) return;
  lv_label_set_text(s_toast_lbl, msg.c_str());
  lv_obj_clear_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_toast_box);
  lv_obj_fade_in(s_toast_box, 200, 0);
  lv_timer_reset(s_toast_tmr);
  lv_timer_resume(s_toast_tmr);
}

void ui_set_backlight_cb(void (*cb)(bool on), bool initial_on) {
  s_bl_cb = cb;
  s_bl_on = initial_on;
  s_dimmed = false;
  if (s_sw_bl) {
    if (initial_on)
      lv_obj_add_state(s_sw_bl, LV_STATE_CHECKED);
    else
      lv_obj_clear_state(s_sw_bl, LV_STATE_CHECKED);
  }
  // v1.8.2: actually apply the persisted preference. The board turns the
  // backlight ON during bring-up; without this, a saved "backlight OFF"
  // only took effect after the user toggled the switch in this boot.
  applyBacklight();
}

void ui_request_feed(uint8_t seconds) {
  if (seconds < 1 || seconds > CATFISH_PULSE_S_LIMIT) {
    uint8_t d = prefs::getDefaultPulse();
    seconds = (d >= 1 && d <= CATFISH_PULSE_S_LIMIT) ? d : CATFISH_DEFAULT_PULSE_S;
  }
  UiEvent ev;
  ev.type = UI_EVT_FEED;
  ev.a = seconds;
  ev.flag = true; // manual
  uiEventSend(ev);
  Serial.printf("[UI] feed requested: %u s\n", (unsigned)seconds);
}

void ui_set_feed_busy(bool busy) {
  if (s_feed_btn) {
    if (busy)
      lv_obj_add_state(s_feed_btn, LV_STATE_DISABLED);
    else
      lv_obj_clear_state(s_feed_btn, LV_STATE_DISABLED);
  }
}

void ui_set_probe_busy(bool busy) {
  if (s_probe_btn) {
    if (busy)
      lv_obj_add_state(s_probe_btn, LV_STATE_DISABLED);
    else
      lv_obj_clear_state(s_probe_btn, LV_STATE_DISABLED);
  }
}

void ui_set_scan_busy(bool busy) {
  if (s_scan_btn) {
    if (busy)
      lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);
    else
      lv_obj_clear_state(s_scan_btn, LV_STATE_DISABLED);
  }
}

void ui_tanks_rebuild(uint8_t count, const String names[CATFISH_MAX_TANKS],
                      uint8_t selected) {
  if (count < 1) count = 1;
  if (count > CATFISH_MAX_TANKS) count = CATFISH_MAX_TANKS;
  s_tank_count = count;
  s_sel_tank = (selected == 0xFE || selected < count) ? selected : 0;
  for (uint8_t i = 0; i < CATFISH_MAX_TANKS; i++) {
    strlcpy(s_tank_names[i], names[i].c_str(), sizeof(s_tank_names[i]));
    if (!s_tank_names[i][0])
      snprintf(s_tank_names[i], sizeof(s_tank_names[i]), "Tank %u",
               (unsigned)(i + 1));
  }
  if (s_tank_sb_count) spinSetValueQuiet(s_tank_sb_count, s_tank_count);
  rebuildTankChips();
  rebuildTankRows();
  rebuildScheduleList(); // entry labels carry tank tags when count > 1
}

void ui_tanks_state(const bool online[CATFISH_MAX_TANKS],
                    const bool feeding[CATFISH_MAX_TANKS],
                    const String ips[CATFISH_MAX_TANKS]) {
  for (uint8_t i = 0; i < s_tank_count && i < CATFISH_MAX_TANKS; i++) {
    if (s_tank_row_dot[i]) {
      lv_obj_set_style_bg_color(s_tank_row_dot[i],
                                lv_color_hex(online[i] ? C_GREEN : C_MUTED), 0);
    }
    if (s_tank_row_ip[i]) {
      if (ips[i].length())
        lv_label_set_text(s_tank_row_ip[i], ips[i].c_str());
      else
        lv_label_set_text(s_tank_row_ip[i], "not found");
    }
  }
  (void)feeding; // chips stay name-only; feeding shows on the Home relay card
}

void ui_schedule_refresh() { rebuildScheduleList(); }

void ui_editor_close() { closeEditor(); }

void ui_set_mqtt_status(const String &text, uint8_t state) {
  if (!s_mq_status) return;
  lv_label_set_text_fmt(s_mq_status, "MQTT: %s", text.c_str());
  uint32_t color = C_MUTED;
  if (state == 1) color = C_AMBER;    // connecting / retrying
  if (state == 2) color = C_GREEN;    // connected
  if (state == 3) color = C_RED;      // error
  lv_obj_set_style_text_color(s_mq_status, lv_color_hex(color), 0);
}

void ui_set_mqtt_broker(const String &text) {
  if (s_mq_broker) lv_label_set_text(s_mq_broker, text.c_str());
}

void ui_set_pulse(uint8_t seconds) {
  spinSetValueQuiet(s_sb_pulse, seconds);
}

void ui_set_hour12(bool h12) {
  s_hour12 = h12;
  buildRollerOptions();
  if (s_r_hour) {
    // the roller's selected INDEX is the 24-hour value — preserve it across
    // the relabel (index semantics are format-independent)
    const uint16_t sel = (uint16_t)lv_roller_get_selected(s_r_hour);
    lv_roller_set_options(s_r_hour, s_hour_opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(s_r_hour, sel, LV_ANIM_OFF);
  }
  if (s_sw_h12) {
    if (h12)
      lv_obj_add_state(s_sw_h12, LV_STATE_CHECKED);
    else
      lv_obj_clear_state(s_sw_h12, LV_STATE_CHECKED);
  }
  // force cached clock/countdown/labels to repaint on the next tick
  s_clock_cache[0] = '\0';
  s_val_cache[0] = '\0';
  s_sub_cache[0] = '\0';
  rebuildScheduleList(); // schedule card labels carry the format too
}

void ui_set_rtc(const String &text) {
  if (s_lbl_rtc) lv_label_set_text_fmt(s_lbl_rtc, "RTC: %s", text.c_str());
}

void ui_set_sd(const String &text) {
  if (s_lbl_sd) lv_label_set_text_fmt(s_lbl_sd, "SD card: %s", text.c_str());
}

void ui_set_radios(const String &espnow, const String &rf24) {
  if (s_lbl_espnow)
    lv_label_set_text_fmt(s_lbl_espnow, "ESP-NOW: %s", espnow.c_str());
  if (s_lbl_rf24)
    lv_label_set_text_fmt(s_lbl_rf24, "nRF24L01: %s", rf24.c_str());
}

void ui_set_mech(const String &mechName, int restDeg, int feedDeg, int stepHz) {
  // cache for the editor overlay prefill
  if (mechName.length()) {
    s_mech_reported = true;
    const uint8_t id = catfishMechFromName(mechName.c_str());
    s_mech_id = (id == 0xFF) ? CATFISH_MECH_DEFAULT : id;
  } else {
    s_mech_reported = false;
    s_mech_id = CATFISH_MECH_DEFAULT;
  }
  s_mech_rest = restDeg;
  s_mech_feed = feedDeg;
  s_mech_hz = stepHz;

  if (!s_lbl_mech) return;
  if (!s_mech_reported) {
    lv_label_set_text(s_lbl_mech, "Mechanism: not reported (update relay)");
    lv_obj_set_style_text_color(s_lbl_mech, lv_color_hex(C_MUTED), 0);
    return;
  }
  char buf[48];
  switch (s_mech_id) {
    case CATFISH_MECH_SERVO:
      snprintf(buf, sizeof(buf), "Mechanism: Servo (%d\xC2\xB0 -> %d\xC2\xB0)", // "→" = tofu in Montserrat
               (s_mech_rest >= 0) ? s_mech_rest : 0, (s_mech_feed >= 0) ? s_mech_feed : 0);
      break;
    case CATFISH_MECH_STEPPER:
      snprintf(buf, sizeof(buf), "Mechanism: Stepper (%d steps/s)",
               (s_mech_hz >= 0) ? s_mech_hz : 0);
      break;
    default:
      snprintf(buf, sizeof(buf), "Mechanism: Motor relay");
      break;
  }
  lv_label_set_text(s_lbl_mech, buf);
  lv_obj_set_style_text_color(s_lbl_mech, lv_color_hex(C_TEXT), 0);
}

// ------------------------------------------------------------------ v1.9.0
void ui_set_vacation(bool on) {
  s_vacation = on;
  if (s_lbl_vacation) {
    if (on)
      lv_obj_clear_flag(s_lbl_vacation, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_lbl_vacation, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_sw_vacation) {
    if (on)
      lv_obj_add_state(s_sw_vacation, LV_STATE_CHECKED);
    else
      lv_obj_clear_state(s_sw_vacation, LV_STATE_CHECKED);
  }
}

void ui_set_food(uint8_t count, const uint8_t pct[CATFISH_MAX_TANKS],
                 const uint16_t usedSec[CATFISH_MAX_TANKS],
                 const uint16_t gps[CATFISH_MAX_TANKS],
                 const uint16_t capSec[CATFISH_MAX_TANKS], uint8_t lowPct) {
  if (count < 1) count = 1;
  if (count > CATFISH_MAX_TANKS) count = CATFISH_MAX_TANKS;
  bool changed = false;
  for (uint8_t i = 0; i < CATFISH_MAX_TANKS; i++) {
    const uint8_t srcIdx = (i < count) ? i : CATFISH_MAX_TANKS - 1;
    (void)srcIdx;
    if (pct[i] != s_food_pct[i]) changed = true;
    s_food_pct[i] = pct[i];
    s_food_used[i] = usedSec[i];
    s_food_gps[i] = gps[i];
    s_food_cap[i] = capSec[i];
  }
  s_food_low = lowPct;
  // rebuild rows when the tank count changed (row layout is per count)
  if (s_food_rows && s_food_rows_count != count) {
    // keep the current tank names in sync via s_tank_names (ui_tanks_rebuild)
    rebuildFoodRows();
    changed = true;
  }
  if (changed || s_food_rows_count == count) refreshFoodRows();
}

void ui_set_history(const String &json) {
  if (!s_hist_lbl) return;
  if (json.length() < 3) {
    lv_label_set_text(s_hist_lbl, "No feed events yet.");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok ||
      !doc.is<JsonArray>()) {
    lv_label_set_text(s_hist_lbl, "No feed events yet.");
    return;
  }
  String out;
  int n = 0;
  for (JsonObject o : doc.as<JsonArray>()) {
    const uint32_t e = (uint32_t)(o["e"] | 0);
    const uint8_t m = (uint8_t)(o["m"] | 1);
    const uint8_t s = (uint8_t)(o["s"] | 0);
    const bool ok = ((o["ok"] | 0) != 0);
    const uint8_t f = (uint8_t)(o["f"] | 0);
    const time_t t = (time_t)e;
    struct tm lt;
    char when[24] = "--/-- --:--";
    if (e != 0 && localtime_r(&t, &lt)) {
      snprintf(when, sizeof(when), "%02d-%02d %02d:%02d", lt.tm_mon + 1,
               lt.tm_mday, lt.tm_hour, lt.tm_min);
    }
    char tanks[20] = {0};
    int ti = 0;
    for (uint8_t b = 0; b < CATFISH_MAX_TANKS; b++) {
      if (m & (1u << b))
        ti += snprintf(tanks + ti, sizeof(tanks) - ti, "%s%u", ti ? "+" : "",
                       (unsigned)(b + 1));
    }
    const char *flag = "";
    switch (f) {
      case 1: flag = " manual"; break;
      case 2: flag = " catch-up"; break;
      case 3: flag = " retry"; break;
      case 4: flag = " VERIFY"; break;
      case 5: flag = " blocked"; break;
      case 6: flag = " skipped"; break;
      default: break;
    }
    char line[64];
    if (ok)
      snprintf(line, sizeof(line), "%s  OK   T%s  %us%s\n", when,
               tanks[0] ? tanks : "1", (unsigned)s, flag);
    else
      snprintf(line, sizeof(line), "%s  FAIL T%s  %us%s\n", when,
               tanks[0] ? tanks : "1", (unsigned)s, flag);
    out += line;
    if (++n >= 24) break;
  }
  lv_label_set_text(s_hist_lbl, out.length() ? out.c_str() : "No feed events yet.");
}

} // namespace ui
