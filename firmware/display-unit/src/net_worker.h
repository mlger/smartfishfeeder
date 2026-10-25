/**
 * @file net_worker.h
 * @brief Background task that owns ALL blocking relay HTTP/discovery work.
 *
 * loop() enqueues commands and never blocks; the worker executes them one at
 * a time (relay_client is single-caller) and posts Results back through a
 * second queue. Keeps feed_scheduler::tick() and the UI cadence punctual
 * even during discovery chains (~5.5 s worst case) or feed POSTs (6 s).
 *
 * Multi-tank (Phase 5): FEED carries a tank bitmask and the worker performs
 * one relay_client::feedTank() per set bit (sequentially), aggregating the
 * per-tank outcomes into the result. PROBE/MECH/TOKEN_ROTATE are addressed
 * to a single 1-based tank id (0 means tank 1).
 */
#ifndef CATFISH_NET_WORKER_H
#define CATFISH_NET_WORKER_H

#include <Arduino.h>
#include <stdint.h>

#include <catfish_protocol.h>

namespace net_worker {

struct Cmd {
  enum Type : uint8_t {
    CMD_FEED = 0,
    CMD_PROBE,
    CMD_TOKEN_ROTATE,
    CMD_MECH,       // P4: switch a relay's feeding mechanism via POST /config
    CMD_SCAN_TANKS, // P5: UDP-scan the LAN and bind unassigned tanks
  };
  Type type = CMD_FEED;
  uint8_t seconds = 0; // FEED: pulse duration (shared / fallback)
  bool manual = false; // FEED: manual vs scheduled
  bool retry = false;  // FEED: this attempt IS the scheduled-feed retry
  bool catchup = false; // v1.9.0 (A4): this attempt is a missed-feed catch-up
  uint8_t hour = 0;    // FEED: scheduled entry time (0 for manual)
  uint8_t minute = 0;
  // v1.9.0 (A2): per-tank pulse override, index t-1, 0 = use `seconds`.
  uint8_t secTank[4] = {0, 0, 0, 0};
  bool report = false; // PROBE: toast the outcome
  bool verify = false; // v1.9.0 (B3): probe runs inside the pulse-verify window
  char token[CATFISH_TOKEN_LEN] = {0}; // TOKEN_ROTATE: new token for the relay
  // Multi-tank (P5)
  uint8_t tankMask = 0; // FEED: target tanks (bit i = tank i+1); 0 = tank 1
  uint8_t tankId = 0;   // PROBE/MECH/TOKEN_ROTATE: 1-based tank; 0 = tank 1
  // MECH (P4): pre-clamped by the UI; relay clamps again
  uint8_t mechId = 0;      // CATFISH_MECH_*
  uint8_t mechRest = 0;    // servo rest angle
  uint8_t mechFeed = 0;    // servo feed angle
  uint16_t mechHz = 0;     // stepper step rate
  uint8_t mechDir = 0;     // stepper DIR level
};

struct Result {
  enum Kind : uint8_t { RES_FEED = 0, RES_PROBE, RES_TOKEN, RES_MECH, RES_SCAN };
  Kind kind = RES_FEED;
  bool ok = false;
  // FEED echo
  bool manual = false;
  bool retry = false;
  bool catchup = false; // v1.9.0 (A4)
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t seconds = 0; // applied pulse seconds (relay may have clamped)
  bool clamped = false; // relay clamp note present
  char note[64] = {0}; // relay clamp note text ("")
  uint8_t tanksOk = 0;   // P5: bitmask of tanks fed successfully
  uint8_t tanksFail = 0; // P5: bitmask of tanks whose feed failed
  // v1.9.0 (A2): per-tank applied seconds (relay may clamp); 0 = not fed
  uint8_t secOk[4] = {0, 0, 0, 0};
  // PROBE / MECH / TOKEN echo
  uint8_t tankId = 0; // 1-based tank this result belongs to
  // PROBE
  bool online = false;
  bool feeding = false;
  bool report = false;
  bool verify = false; // v1.9.0 (B3): result is inside the verify window
  bool protoMismatch = false; // relay "proto" != CATFISH_PROTOCOL_VERSION
  char mech[10] = {0}; // relay "mechanism" ("motor"/"servo"/"stepper"; "" = pre-P4)
  int16_t mechRest = -1; // servo rest angle from /status (-1 = unknown/pre-P4)
  int16_t mechFeed = -1; // servo feed angle from /status
  int16_t mechHz = -1;   // stepper step rate from /status
  // TOKEN_ROTATE echo
  char token[CATFISH_TOKEN_LEN] = {0};
  // SCAN
  uint8_t boundMask = 0;  // tanks that got a NEW binding
  uint8_t liveCount = 0;  // tanks with a live IP after the scan
  char ip[16] = {0};
  char version[16] = {0};
  char error[48] = {0};
};

/** Create the command/result queues + worker task (core 0, prio 2). */
bool begin();

/**
 * Queue a feed request. @p tankMask bit i = tank i+1; 0 = tank 1.
 * @p secTank per-tank pulse override (0 = use @p seconds), may be nullptr.
 * @return false when the command queue is full.
 */
bool enqueueFeed(uint8_t seconds, bool manual, uint8_t hour, uint8_t minute,
                 bool retry, uint8_t tankMask, const uint8_t *secTank = nullptr,
                 bool catchup = false);

/** Queue a per-tank relay probe (discover-if-needed + GET /status). */
bool enqueueProbe(bool report, uint8_t tank1, bool verify = false);

/**
 * Ask a tank's relay to rotate its token to @p newToken via POST /config.
 * On success the loop persists the new token locally.
 */
bool enqueueTokenRotate(const char *newToken, uint8_t tank1);

/**
 * P4: switch a tank relay's feeding mechanism (POST /config "mechanism" +
 * parameters). Values are clamped by catfish_mech.h on both ends.
 */
bool enqueueMech(uint8_t mechId, uint8_t restDeg, uint8_t feedDeg,
                 uint16_t stepHz, uint8_t stepDir, uint8_t tank1);

/** P5: UDP scan + bind tanks that have no live relay IP. */
bool enqueueScanTanks();

/** Non-blocking result drain for loop(). @return false when empty. */
bool receive(Result &out);

} // namespace net_worker

#endif // CATFISH_NET_WORKER_H
