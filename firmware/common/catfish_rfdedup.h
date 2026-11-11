/**
 * @file catfish_rfdedup.h
 * @brief Catfish Feeder — direct puck feed dedup state machine (v1.13.0).
 *
 * Background: with v1.13.0 a relay unit (ESP8266) can listen to the wireless
 * feed-node datagram (catfish_rfnode.h) DIRECTLY — "espnowDirect" mode — so a
 * puck press still feeds when the router (and therefore the display hub's LAN
 * path) is down. When the router is UP, both the hub AND the relay hear the
 * same broadcast; the hub resolves it and POSTs /feed. Without coordination
 * the fish would be fed twice.
 *
 * Coordination contract (hub >= 1.13.0):
 *   1. The hub echoes the originating datagram sequence number in the /feed
 *      POST ("seq" field). A seq of 0 means "hub-initiated feed, no datagram
 *      involved" (touch / dashboard / MQTT / scheduled).
 *   2. The relay arms a PENDING slot when a valid datagram arrives and waits
 *      CATFISH_RFDEDUP_GRACE_MS. If ANY authenticated /feed arrives within the
 *      grace window (hub echo or any other hub feed), the pending intent is
 *      cancelled — the hub is alive and owns feeding.
 *   3. If the grace window expires with no /feed, the relay feeds DIRECTLY
 *      from the datagram (its own default pulse when seconds == 0), and the
 *      seq lands in a small executed-ring so a re-broadcast of the same seq
 *      is never fed twice.
 *
 * This header is dependency-free and host-testable: the native suite
 * (test_catfish_rfdedup) pins the exact behaviour both units rely on.
 * The display unit keeps its own simpler seq ring (net_espnow.cpp) — it never
 * needs the grace dance because it is the authority, not the peer.
 */
#ifndef CATFISH_RFDEDUP_H
#define CATFISH_RFDEDUP_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/** Executed-seq ring size (mirror of the hub's net_espnow SEQ_RING). */
#define CATFISH_RFDEDUP_RING 8

/** How long the relay waits for the hub's /feed echo before feeding directly. */
#define CATFISH_RFDEDUP_GRACE_MS 1500

enum CatfishRfDedupAction : uint8_t {
  CATFISH_RFDEDUP_NONE = 0, // keep waiting / nothing to do
  CATFISH_RFDEDUP_FEED = 1, // grace expired -> feed directly from the datagram
};

struct CatfishRfDedup {
  uint16_t ring[CATFISH_RFDEDUP_RING]; // executed seqs (both paths)
  uint8_t ringIdx;
  bool hasPending;
  uint16_t pendingSeq;
  uint8_t pendingSeconds; // datagram seconds (0 = relay default pulse)
  uint32_t pendingAtMs;   // millis() the pending slot was armed
};

inline void catfishRfDedupInit(CatfishRfDedup *d) {
  memset(d, 0, sizeof(*d));
  // Ring slots pre-filled with 0xFFFF: seq 0 is never a datagram seq in the
  // executed sense (hub-only feeds carry seq 0), but a zero ring slot would
  // match the very first seq==0 arm — same class of bug the hub's ring had
  // in v1.8.2. 0xFFFF is a legal seq, so also treat 0 as "never executed".
  for (size_t i = 0; i < CATFISH_RFDEDUP_RING; i++) d->ring[i] = 0xFFFF;
}

/** True when @p seq was already fed (direct or via the hub's seq echo). */
inline bool catfishRfDedupRingHas(const CatfishRfDedup *d, uint16_t seq) {
  if (seq == 0) return true; // 0 is not a real datagram seq; always "seen"
  for (size_t i = 0; i < CATFISH_RFDEDUP_RING; i++) {
    if (d->ring[i] == seq) return true;
  }
  return false;
}

inline void catfishRfDedupRingAdd(CatfishRfDedup *d, uint16_t seq) {
  if (seq == 0) return;
  d->ring[d->ringIdx] = seq;
  d->ringIdx = (uint8_t)((d->ringIdx + 1) % CATFISH_RFDEDUP_RING);
}

/**
 * Arm a pending direct-feed intent for @p seq.
 * @return false when @p seq is already in the executed ring (caller drops the
 *         datagram entirely). Re-arming the SAME pending seq just refreshes
 *         the timestamp (a puck re-broadcast extends the grace window) —
 *         arming a DIFFERENT seq while one is pending replaces it (single
 *         feeder; last press wins).
 */
inline bool catfishRfDedupArm(CatfishRfDedup *d, uint16_t seq, uint8_t seconds,
                              uint32_t nowMs) {
  if (catfishRfDedupRingHas(d, seq)) return false;
  d->hasPending = true;
  d->pendingSeq = seq;
  d->pendingSeconds = seconds;
  d->pendingAtMs = nowMs;
  return true;
}

/** Any authenticated /feed cancels the pending direct intent (hub is alive). */
inline void catfishRfDedupCancelPending(CatfishRfDedup *d) {
  d->hasPending = false;
}

/**
 * Poll the pending slot. When the grace window has expired, return
 * CATFISH_RFDEDUP_FEED, add the seq to the executed ring and clear the
 * pending slot; @p outSeq/@p outSeconds receive the datagram values. Callers
 * must still honour the pulse-engine busy guard at execution time.
 */
inline uint8_t catfishRfDedupPoll(CatfishRfDedup *d, uint32_t nowMs,
                                  uint16_t *outSeq, uint8_t *outSeconds) {
  if (!d->hasPending) return CATFISH_RFDEDUP_NONE;
  if ((int32_t)(nowMs - d->pendingAtMs) < CATFISH_RFDEDUP_GRACE_MS) {
    return CATFISH_RFDEDUP_NONE; // wrap-safe comparison
  }
  d->hasPending = false;
  catfishRfDedupRingAdd(d, d->pendingSeq);
  if (outSeq) *outSeq = d->pendingSeq;
  if (outSeconds) *outSeconds = d->pendingSeconds;
  return CATFISH_RFDEDUP_FEED;
}

#endif // CATFISH_RFDEDUP_H
