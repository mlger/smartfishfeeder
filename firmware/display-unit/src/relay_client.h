/**
 * @file relay_client.h
 * @brief LAN HTTP client for the ESP8266 relay units (multi-tank, Phase 5).
 *
 * One display unit can drive up to CATFISH_MAX_TANKS relay units. Every call
 * is addressed by a 1-based tank id; each tank keeps its own cached IP and
 * transport-failure streak. Tank 1 is the legacy single-relay slot, so a
 * single-tank setup behaves exactly like pre-Phase-5 firmware.
 *
 * Discovery per tank:
 *   1. Saved IP validated with GET /health (2 s timeout)
 *   2. One UDP broadcast round (1.5 s) collecting ALL responders; the first
 *      candidate not already bound to ANOTHER tank is bound to this tank
 *      (mDNS "catfish-relay" is included as an extra candidate; note that
 *      multiple relays share that hostname, so UDP is the authoritative
 *      multi-relay discovery)
 *   3. Fail
 *
 * scanAndBindTanks() runs one broadcast round and re-binds every configured
 * tank that has no live IP — used by Settings > TANKS > "Scan for relays".
 *
 * Tokens: the net worker task must never parse prefs JSON/blob state, so
 * applyTankTokens() (loop context) snapshots the effective per-tank tokens
 * into a spinlock-guarded table the worker reads.
 *
 * A successful discovery is cached and persisted to NVS (prefs tank keys).
 */
#ifndef CATFISH_RELAY_CLIENT_H
#define CATFISH_RELAY_CLIENT_H

#include <Arduino.h>
#include <stdint.h>

#include <catfish_protocol.h>

namespace relay_client {

constexpr int MAX_TANKS = CATFISH_MAX_TANKS;

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
  String mechanism = "";    // "motor"/"servo"/"stepper"; "" = relay firmware without P4
  int servoRest = -1;       // mechanism params from /status (-1 = unknown/pre-P4)
  int servoFeed = -1;
  int stepHz = -1;
  String error = "";
};

/** Load per-tank IPs from prefs. Call once in setup(). */
void init();

/**
 * Loop-context snapshot of the effective per-tank tokens (per-tank override
 * or global pairing token). Call after any token/tank-config change; the net
 * worker reads the guarded copies.
 */
void applyTankTokens();

/** true when tank @p tank1 (1-based) has a cached IP. */
bool hasIp(uint8_t tank1);

/** Cached relay IP for a tank ("" if unknown). */
String ip(uint8_t tank1);

/** Cache + persist a tank's IP (loop or worker context; NVS write is fine in both). */
void setTankIp(uint8_t tank1, const String &ip);

/** Drop a tank's cached IP (persisted), e.g. after repeated transport failures. */
void clearTankIp(uint8_t tank1);

/** Run the per-tank discovery chain; binds + persists the IP on success. */
Result discoverTank(uint8_t tank1);

/**
 * POST /feed with {"seconds":n,"token":<tank token>} (6 s timeout).
 * Auto-discovers first if the tank has no IP. A transport failure with a
 * cached IP triggers one re-discovery + retry (stale-IP self-heal).
 * Blocking — call from the net worker task only.
 */
Result feedTank(uint8_t tank1, uint8_t seconds);

/**
 * GET /status for a tank -> parse {"state","feeding","lastFeedEpoch",
 * "version","proto","mechanism",...}. Protocol mismatch warns once per boot.
 */
Status fetchStatusTank(uint8_t tank1);

/**
 * POST /config switching the feeding mechanism on a tank's relay (Phase 4).
 * Values are pre-clamped by the shared catfish_mech.h helpers; the relay
 * clamps again (belt and braces). Call from the net worker task only.
 */
Result configureMechTank(uint8_t tank1, uint8_t mech, uint8_t restDeg,
                         uint8_t feedDeg, uint16_t stepHz, uint8_t stepDir);

/**
 * POST /config asking a tank's relay to rotate its token to @p newToken.
 * Authorizes with the tank's CURRENT effective token, so this only succeeds
 * while display and relay are still in sync. On success the caller must
 * persist it (prefs::setTankToken) and refresh applyTankTokens().
 */
Result rotateTokenTank(uint8_t tank1, const String &newToken);

/**
 * One UDP broadcast round; bind every configured tank that has no live IP to
 * a relay not claimed by another tank. @return tanks with a live IP after the
 * scan; @p assignedMask (may be nullptr) has bit i set when tank i+1 got a
 * NEW binding. Blocking — call from the net worker task only.
 */
int scanAndBindTanks(uint8_t tankCount, uint8_t *assignedMask);

} // namespace relay_client

#endif // CATFISH_RELAY_CLIENT_H
