/**
 * @file net_espnow.h
 * @brief Optional ESP-NOW receiver for wireless feed nodes (v1.8.0).
 *
 * Battery pucks (ESP8266/ESP32 + button) broadcast the shared Catfish RF-node
 * datagram (catfish_rfnode.h) on the hub's WiFi channel; the display unit
 * listens and routes accepted requests into the SAME feed pipeline as touch
 * input (UiEvent queue -> loop() -> net worker). The relay unit is untouched.
 *
 * Trust: packet key must equal FNV-1a(pairing token). Token rotations call
 * onTokenChanged() from loop() so the cached key stays current without
 * cross-task prefs reads.
 *
 * Feature gate: -DCATFISH_FEATURE_ESPNOW=1 (default). Started lazily once
 * WiFi STA is connected (so the listen channel matches the AP). Nodes must
 * transmit on that same channel.
 */
#ifndef CATFISH_ESPNOW_H
#define CATFISH_ESPNOW_H

#include <Arduino.h>
#include <stdint.h>

#ifndef CATFISH_FEATURE_ESPNOW
#define CATFISH_FEATURE_ESPNOW 1
#endif

namespace net_espnow {

enum State : uint8_t {
  Disabled = 0, // compiled out
  WaitingWifi,  // compiled in, not started yet (no WiFi)
  Listening,
  Error,
};

State state();

/**
 * Lazy start — call from loop() once WiFi is connected (repeat calls are
 * cheap no-ops after the first success).
 */
bool beginOnce();

/** Re-derive the packet key from the pairing token (loop() context). */
void onTokenChanged();

} // namespace net_espnow

#endif // CATFISH_ESPNOW_H
