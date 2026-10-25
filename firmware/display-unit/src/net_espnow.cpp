/**
 * @file net_espnow.cpp
 * @brief Optional ESP-NOW receiver for wireless feed nodes (v1.8.0).
 *
 * The recv callback runs in the WiFi task context: it does NO allocation,
 * no prefs reads, no LVGL — just parse + key check + a zero-timeout
 * ui_event_send() (the same cross-task entry the MQTT manager uses).
 */
#include "net_espnow.h"

#if CATFISH_FEATURE_ESPNOW

#include <WiFi.h>
#include <esp_now.h>

#include <catfish_rfnode.h>

#include "prefs.h"
#include "ui/ui.h"

namespace {

net_espnow::State s_state = net_espnow::State::WaitingWifi;
bool s_started = false;
uint32_t s_key = 0;
uint32_t s_accepted = 0;
uint32_t s_rejected = 0;

// Small dedup ring: a puck retry often re-broadcasts the same seq.
constexpr size_t SEQ_RING = 8;
// Pre-filled with 0xFFFF (v1.8.2): a {0}-initialized ring permanently
// rejected the very first seq==0 broadcast of any node (a zero slot
// always 'matched' and was never replaced).
uint16_t s_seen[SEQ_RING] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                             0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
uint8_t s_seen_idx = 0;

bool seqSeen(uint16_t seq) {
  for (size_t i = 0; i < SEQ_RING; i++) {
    if (s_seen[i] == seq) return true;
  }
  s_seen[s_seen_idx] = seq;
  s_seen_idx = (uint8_t)((s_seen_idx + 1) % SEQ_RING);
  return false;
}

void rederiveKey() {
  s_key = catfishRfnodeKeyFromToken(prefs::getRelayToken().c_str());
}

void recvCb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info; // sender MAC intentionally not filtered — key is the gate
  uint8_t cmd = 0xFF, mask = 0, seconds = 0;
  uint16_t seq = 0;
  uint32_t key = 0;
  if (!catfishRfnodeParse(data, (size_t)len, &cmd, &mask, &seconds, &seq,
                          &key)) {
    s_rejected++;
    return;
  }
  if (key != s_key) {
    s_rejected++;
    Serial.printf("[ESPNOW] rejected node packet (bad key, seq %u)\n",
                  (unsigned)seq);
    return;
  }
  if (seqSeen(seq)) {
    s_rejected++;
    return; // duplicate broadcast
  }
  s_accepted++;

  UiEvent ev;
  ev.type = UI_EVT_RF_FEED;
  // v1.10.0: the datagram still carries seconds (wire compat) — convert to
  // the UI's grams unit here using tank 1's calibrated rate.
  ev.grams = catfishSecondsToGrams(seconds, prefs::getTankGps(1));
  ev.tankMask = mask;
  ui::ui_event_send(ev); // zero-timeout; drops silently when the queue is full
  Serial.printf("[ESPNOW] node feed request (%u s ~= %u g, mask 0x%X, seq %u)\n",
                (unsigned)seconds, (unsigned)ev.grams, (unsigned)mask,
                (unsigned)seq);
}

} // anonymous namespace

namespace net_espnow {

State state() { return s_state; }

bool beginOnce() {
  if (s_started) return true;
  rederiveKey();
  if (esp_now_init() != ESP_OK) {
    s_state = State::Error;
    Serial.println("[ESPNOW] init FAILED");
    return false;
  }
  esp_now_register_recv_cb(recvCb);
  s_started = true;
  s_state = State::Listening;
  Serial.println("[ESPNOW] listening for feed nodes (channel follows WiFi)");
  return true;
}

void onTokenChanged() {
  if (s_started) rederiveKey();
}

} // namespace net_espnow

#else // !CATFISH_FEATURE_ESPNOW

namespace net_espnow {
State state() { return State::Disabled; }
bool beginOnce() { return false; }
void onTokenChanged() {}
} // namespace net_espnow

#endif // CATFISH_FEATURE_ESPNOW
