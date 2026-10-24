/**
 * @file net_worker.h
 * @brief Background task that owns ALL blocking relay HTTP/discovery work.
 *
 * loop() enqueues commands and never blocks; the worker executes them one at
 * a time (relay_client is single-caller) and posts Results back through a
 * second queue. Keeps feed_scheduler::tick() and the UI cadence punctual
 * even during discovery chains (~5.5 s worst case) or feed POSTs (6 s).
 */
#ifndef CATFISH_NET_WORKER_H
#define CATFISH_NET_WORKER_H

#include <Arduino.h>
#include <stdint.h>

#include <catfish_protocol.h>

namespace net_worker {

struct Cmd {
  enum Type : uint8_t { CMD_FEED = 0, CMD_PROBE, CMD_TOKEN_ROTATE };
  Type type = CMD_FEED;
  uint8_t seconds = 0; // FEED: pulse duration
  bool manual = false; // FEED: manual vs scheduled
  bool retry = false;  // FEED: this attempt IS the scheduled-feed retry
  uint8_t hour = 0;    // FEED: scheduled entry time (0 for manual)
  uint8_t minute = 0;
  bool report = false; // PROBE: toast the outcome
  char token[CATFISH_TOKEN_LEN] = {0}; // TOKEN_ROTATE: new token for the relay
};

struct Result {
  enum Kind : uint8_t { RES_FEED = 0, RES_PROBE, RES_TOKEN };
  Kind kind = RES_FEED;
  bool ok = false;
  // FEED echo
  bool manual = false;
  bool retry = false;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t seconds = 0; // applied pulse seconds (relay may have clamped)
  bool clamped = false; // relay clamp note present
  char note[64] = {0}; // relay clamp note text ("")
  // PROBE
  bool online = false;
  bool feeding = false;
  bool report = false;
  // TOKEN_ROTATE echo
  char token[CATFISH_TOKEN_LEN] = {0};
  char ip[16] = {0};
  char version[16] = {0};
  char error[48] = {0};
};

/** Create the command/result queues + worker task (core 0, prio 2). */
bool begin();

/** Queue a feed request. @return false when the command queue is full. */
bool enqueueFeed(uint8_t seconds, bool manual, uint8_t hour, uint8_t minute,
                 bool retry);

/** Queue a relay probe (discover-if-needed + GET /status). */
bool enqueueProbe(bool report);

/**
 * Ask the relay to rotate its token to @p newToken via POST /config.
 * On success the loop persists the new token locally.
 */
bool enqueueTokenRotate(const char *newToken);

/** Non-blocking result drain for loop(). @return false when empty. */
bool receive(Result &out);

} // namespace net_worker

#endif // CATFISH_NET_WORKER_H
