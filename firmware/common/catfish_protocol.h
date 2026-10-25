/**
 * @file catfish_protocol.h
 * @brief Catfish Feeder — shared protocol constants (display unit <-> relay unit).
 *
 * Single source of truth for everything both firmwares must agree on:
 * discovery strings/ports, mDNS hostnames, pairing token default + capacity,
 * pulse limits, and HTTP endpoint paths.
 *
 * Included by both PlatformIO projects via `-I ../common` in their
 * platformio.ini build_flags. Keep this header dependency-free (no Arduino
 * includes) so it stays usable for host-side tooling/tests later.
 */
#pragma once

// ------------------------------------------------------------ firmware version
// Single source of truth for BOTH units (display + relay derive their version
// strings from this). The #ifndef guard lets an external -DCATFISH_APP_VERSION
// override it without redefinition warnings.
#ifndef CATFISH_APP_VERSION
#define CATFISH_APP_VERSION "1.10.0"
#endif

// ------------------------------------------------------------ protocol version
// Wire-contract revision: bump when endpoints, JSON fields, discovery strings
// or pairing semantics change in a way that requires BOTH units to agree.
// The relay reports this in GET /status as "proto"; the display parses it and
// warns when the two firmwares disagree (e.g. only one unit was re-flashed).
#define CATFISH_PROTOCOL_VERSION 1

// ------------------------------------------------------------ MQTT / Home Assistant
// The DISPLAY unit is the MQTT hub (the relay stays HTTP-only so the ESP8266
// budget is protected). State + command topics live under CATFISH_MQTT_PREFIX;
// HA discovery configs are published (retained) to the standard
// "homeassistant/" prefix. Field caps keep prefs, UI buffers and this header
// in sync.
#define CATFISH_MQTT_PREFIX     "catfish" // default state/command topic root
#define CATFISH_MQTT_PREFIX_LEN 16        // storage capacity incl. NUL
#define CATFISH_MQTT_PORT       1883
#define CATFISH_MQTT_HOST_LEN   64        // host/IP capacity incl. NUL
#define CATFISH_MQTT_USER_LEN   32
#define CATFISH_MQTT_PASS_LEN   64

// ------------------------------------------------------------ discovery (LAN UDP broadcast)
#define CATFISH_DISCOVERY_UDP_PORT   5050
#define CATFISH_DISCOVERY_REQ        "CATFISH_DISCOVER"     // exact-match request
#define CATFISH_DISCOVERY_RSP_PREFIX "CATFISH_RELAY|"       // reply: prefix + <ip>

// ------------------------------------------------------------ mDNS hostnames
#define CATFISH_RELAY_HOSTNAME       "catfish-relay"        // relay advertises; display queries
#define CATFISH_DISPLAY_HOSTNAME     "catfishfeeder"        // display advertises

// ------------------------------------------------------------ pairing token
// Shipped product default (LAN-only gate, not a secret). Rotate on-site via
// the display Settings screen or POST /config {"token":..,"newToken":..}.
#define CATFISH_DEFAULT_TOKEN        "catfish"
#define CATFISH_TOKEN_LEN            24  // storage capacity incl. NUL

// ------------------------------------------------------------ pulse safety (seconds)
#define CATFISH_MAX_PULSE_LIMIT_S    60  // HARD cap, enforced on both units
#define CATFISH_PULSE_S_LIMIT        30  // max editable default/entry pulse
#define CATFISH_DEFAULT_PULSE_S      3   // factory default pulse

// ------------------------------------------------------------ portion safety (grams, v1.10.0)
// v1.10.0: the USER-FACING portion unit is GRAMS everywhere (touch UI, schedule
// editor, web dashboard, history). The wire protocol to the relay stays in
// SECONDS (relay firmware unchanged); the display converts grams -> seconds
// per tank using the tank's calibrated grams-per-second rate (prefs tGpsN).
#define CATFISH_MAX_GRAMS            500 // HARD cap on any portion
#define CATFISH_DEFAULT_GRAMS        15  // factory default portion (3 s @ 5 g/s)
#define CATFISH_DEFAULT_GPS          5   // nominal g/s when a tank has no rate set

/** Clamp a portion to the sane range (pure, host-testable). */
static inline uint16_t catfishClampGrams(uint16_t grams) {
  if (grams < 1) grams = 1;
  if (grams > CATFISH_MAX_GRAMS) grams = CATFISH_MAX_GRAMS;
  return grams;
}

/**
 * Convert a portion in grams to a relay pulse in seconds (ceil, clamped to
 * the wire hard cap). A gps of 0 (= "not calibrated") falls back to the
 * nominal CATFISH_DEFAULT_GPS. Pure, host-testable.
 */
static inline uint8_t catfishGramsToSeconds(uint16_t grams, uint16_t gps) {
  if (gps == 0) gps = CATFISH_DEFAULT_GPS;
  uint32_t s = ((uint32_t)catfishClampGrams(grams) + gps - 1u) / gps;
  if (s < 1u) s = 1u;
  if (s > CATFISH_MAX_PULSE_LIMIT_S) s = CATFISH_MAX_PULSE_LIMIT_S;
  return (uint8_t)s;
}

/** Inverse of catfishGramsToSeconds (seconds -> approximate grams). Pure. */
static inline uint16_t catfishSecondsToGrams(uint8_t seconds, uint16_t gps) {
  if (gps == 0) gps = CATFISH_DEFAULT_GPS;
  uint32_t g = (uint32_t)seconds * gps;
  if (g > CATFISH_MAX_GRAMS) g = CATFISH_MAX_GRAMS;
  return (uint16_t)g;
}

// ------------------------------------------------------------ multi-tank (Phase 5)
// One display unit can drive up to CATFISH_MAX_TANKS relay units. Each tank
// has a display name and an optional token override ("" = inherit the global
// pairing token). Schedules address tanks through a bitmask (bit 0 = tank 1).
#define CATFISH_MAX_TANKS        4
#define CATFISH_TANK_NAME_LEN    16 // storage capacity incl. NUL (15 chars)
#define CATFISH_MASK_ALL_CONFIGURED 0 // special mask: "every configured tank"

// ------------------------------------------------------------ HTTP endpoints (relay unit)
#define CATFISH_EP_HEALTH            "/health"
#define CATFISH_EP_STATUS            "/status"
#define CATFISH_EP_FEED              "/feed"
#define CATFISH_EP_CONFIG            "/config"
