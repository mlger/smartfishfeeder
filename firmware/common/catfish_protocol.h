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
#define CATFISH_APP_VERSION "1.1.0"
#endif

// ------------------------------------------------------------ protocol version
// Wire-contract revision: bump when endpoints, JSON fields, discovery strings
// or pairing semantics change in a way that requires BOTH units to agree.
// The relay reports this in GET /status as "proto"; the display parses it and
// warns when the two firmwares disagree (e.g. only one unit was re-flashed).
#define CATFISH_PROTOCOL_VERSION 1

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

// ------------------------------------------------------------ HTTP endpoints (relay unit)
#define CATFISH_EP_HEALTH            "/health"
#define CATFISH_EP_STATUS            "/status"
#define CATFISH_EP_FEED              "/feed"
#define CATFISH_EP_CONFIG            "/config"
