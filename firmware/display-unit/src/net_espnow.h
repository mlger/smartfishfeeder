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
 * the RADIO is up (v1.12.1 — previously only after WiFi connected, which
 * left the pucks dead whenever the router was off). While the link is down
 * the wifi task pins the last AP channel (catfishRfnodeListenChannel,
 * host-tested), so nodes keep matching it; nodes must transmit on the AP's
 * channel (or channel 1 if the hub never joined a network).
 *
 * v1.14.0: the hub also SENDS now — discoverRelays() broadcasts token-keyed
 * DISCOVER datagrams so the offline "Scan for relays" can find relays with
 * the router off (the relay answers with its ESP-NOW MAC + capabilities;
 * a UDP LAN broadcast cannot exist without WiFi).
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

/** One relay discovered by the offline ESP-NOW scan (v1.14.0). */
struct EspnowRelay {
  uint8_t mac[6];   // relay's ESP-NOW MAC (from the RSP's source address)
  uint8_t tankMask; // relay's direct-feed tank filter (0 = accepts all)
  uint8_t pulseS;   // relay's default pulse seconds (0 = factory default)
  uint16_t proto;   // CATFISH_PROTOCOL_VERSION the relay speaks
};

State state();

/**
 * Lazy start — call from loop() once WiFi is connected (repeat calls are
 * cheap no-ops after the first success).
 */
bool beginOnce();

/** Re-derive the packet key from the pairing token (loop() context). */
void onTokenChanged();

/**
 * v1.14.0: offline relay discovery — broadcast DISCOVER REQ bursts on the
 * current radio channel for @p windowMs and collect token-keyed
 * DISCOVER_RSPs (same trust gate as feed datagrams). BLOCKING by design —
 * call from the net worker task only (it is the ESP-NOW twin of the UDP
 * broadcast round). @return relays written into @p out (up to @p maxOut).
 */
int discoverRelays(uint32_t windowMs, EspnowRelay *out, int maxOut);

} // namespace net_espnow

#endif // CATFISH_ESPNOW_H
