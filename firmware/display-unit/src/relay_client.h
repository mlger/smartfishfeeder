/**
 * @file relay_client.h
 * @brief LAN HTTP client for the ESP8266 relay (feeder motor) unit.
 *
 * Discovery order:
 *   1. Saved IP validated with GET /health (2 s timeout)
 *   2. mDNS query "catfish-relay" (display unit must MDNS.begin("catfishfeeder"))
 *   3. UDP broadcast "CATFISH_DISCOVER\n" to 255.255.255.255:5050,
 *      listening 1500 ms for a "CATFISH_RELAY|<ip>" reply
 *   4. Fail
 *
 * A successful discovery is cached and persisted to NVS.
 */
#ifndef CATFISH_RELAY_CLIENT_H
#define CATFISH_RELAY_CLIENT_H

#include <Arduino.h>
#include <stdint.h>

namespace relay_client {

struct Result {
  bool ok = false;
  String ip = "";
  String error = "";
  // FEED success extras: what the relay actually applied (it may clamp)
  uint8_t seconds_applied = 0;
  bool clamped = false;
  String note = ""; // relay "note" text when the request was clamped
};

struct Status {
  bool ok = false;
  bool feeding = false;
  uint32_t lastFeedEpoch = 0;
  String version = "";
  String state = "";
  int protocolVersion = -1; // relay "proto" field; -1 = old firmware that does not report it
  String error = "";
};

/** Load last-known relay IP from prefs. Call once in setup(). */
void init();

/** true when an IP is cached (from prefs or discovery). */
bool hasIp();

/** Currently cached relay IP ("" if unknown). */
String ip();

/** Run the full discovery chain; persists the IP on success. */
Result discover();

/**
 * POST /feed with {"seconds":n,"token":...} (6 s timeout).
 * Auto-discovers first if no IP is cached. A transport failure with a cached
 * IP triggers one re-discovery + retry (stale-IP self-heal). Blocking — call
 * from the net worker task only.
 */
Result feed(uint8_t seconds);

/**
 * POST /config asking the relay to rotate its token to @p newToken.
 * The relay authorizes with the CURRENT token (fetched from prefs), so this
 * only succeeds while display and relay are still in sync. On success the
 * caller must persist the new token locally (prefs::setRelayToken).
 */
Result rotateToken(const String &newToken);

/**
 * GET /status -> parse {"state","feeding","lastFeedEpoch","version","proto",...}.
 * A protocol-version mismatch is logged once per boot here; callers can also
 * read protocolVersion to surface it in the UI.
 */
Status fetchStatus();

} // namespace relay_client

#endif // CATFISH_RELAY_CLIENT_H
