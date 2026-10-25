/**
 * @file feed_state.h
 * @brief Catfish Feeder — persistent feeding state + policy (v1.9.0).
 *
 * Owns everything the plain scheduler doesn't:
 *   - B1: persistent retry journal for failed scheduled feeds (NVS-backed,
 *         survives power cycles; 3 retries with 30 s / 5 min / 15 min backoff)
 *   - E4: skip-next marker (skip one scheduled occurrence once)
 *   - A4: missed-feed catch-up scan over the fired-map (NVS-backed)
 *   - C3: daily feed counters (ok / fail today + lifetime totals)
 *   - F3: feed policy guards (vacation A5, min-gap, daily cap)
 *   - D1: software hopper estimator (pulse-seconds used since last refill)
 *   - C1: RAM history ring for the on-device history view + /api/history
 *         (the durable copy remains the SD CSV when a card is mounted)
 *
 * Threading: ALL functions are called from loop()/setup() context only
 * (single writer, same discipline as feed_scheduler / prefs). Persistence is
 * immediate per mutation (NVS writes are tiny and rare: per feed event, not
 * per second).
 *
 * Native-testable: pure logic + prefs persistence; time is always passed IN
 * (unix epoch) — no clock reads inside the module. scanCatchup() only uses
 * the host localtime routines to turn an epoch into a weekday.
 */
#ifndef CATFISH_FEED_STATE_H
#define CATFISH_FEED_STATE_H

#include <Arduino.h>
#include <stdint.h>

#include <feed_scheduler.h>

namespace feed_state {

// ---------------------------------------------------------------- sources
enum FeedSource : uint8_t {
  SRC_TOUCH = 0,   // on-device button / wireless node button (a human is there)
  SRC_REMOTE = 1,  // web dashboard / MQTT / HTTP API
  SRC_SCHED = 2,   // scheduled entry
  SRC_CATCHUP = 3, // catch-up of a missed scheduled entry
};

// ---------------------------------------------------------- history flags
enum HistFlag : uint8_t {
  HIST_NONE = 0,
  HIST_MANUAL,   // manual feed (touch/remote/node)
  HIST_CATCHUP,  // catch-up of a missed entry
  HIST_RETRY,    // retry of a failed scheduled feed
  HIST_VERIFY,   // post-feed verify reported "not pulsing"
  HIST_BLOCKED,  // blocked by policy (vacation / gap / daily cap)
  HIST_SKIPPED,  // skipped once via "skip next"
};

struct HistEntry {
  uint32_t epoch = 0;
  uint8_t tankMask = 0;
  uint8_t seconds = 0;
  bool ok = false;
  uint8_t flag = HIST_NONE;
};

// --------------------------------------------------------- lifecycle
/** Load journal / fired-map / counters / hopper from prefs. Call once in setup(). */
void init();

/**
 * Test hook: replace the wall-clock source (nullptr restores system time).
 * Used by the native (host) unit tests; production code never calls it.
 */
void set_time_source(time_t (*fn)());

// ------------------------------------------- B1: persistent retry journal
/** One due retry handed to the 1 Hz fire path. */
struct RetryDue {
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t seconds = 0;
  uint8_t tankMask = 0;
};

/**
 * Arm (or refresh) the journal after a FAILED scheduled/catch-up attempt.
 * A fresh slot starts at attempts=1 with the first backoff (30 s); re-arming
 * an existing (h,m) slot advances the attempt counter — see journalFail().
 * @return false when both slots are full AND no slot matches (h,m).
 */
bool journalArm(uint8_t hour, uint8_t minute, uint8_t seconds,
                uint8_t tankMask);

/**
 * @return true when a slot is due at @p nowEpoch (out filled; the slot goes
 * "in flight" — dueEpoch=0 — until journalClearFor/journalFail resolves it).
 * Slots restored from NVS with a past due date fire on the first call.
 */
bool journalDue(uint32_t nowEpoch, RetryDue &out);

/** Clear the slot(s) for an HH:MM identity (feed succeeded). */
void journalClearFor(uint8_t hour, uint8_t minute);

/**
 * Resolve a failed retry attempt: attempts++ -> next backoff, or deactivate
 * after the last attempt.
 * @return true when the slot stays armed (another retry is scheduled),
 *         false when the journal gave up on this entry.
 */
bool journalFail(uint8_t hour, uint8_t minute);

/** Pending (armed or in-flight) retry count — for status surfaces. */
int journalCount();

// ------------------------------------------------------- E4: skip-next
/** Mark one HH:MM occurrence to be skipped exactly once. */
void setSkipNext(uint8_t hour, uint8_t minute);

/** @return true (+ clears the marker) when this HH:MM was marked skip-once. */
bool consumeSkip(uint8_t hour, uint8_t minute);

// ------------------------------------------------------ A4: catch-up
struct CatchupDue {
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t seconds = 0;
  uint8_t tankMask = 0;
};

/**
 * Scan for missed entries: enabled entries whose most recent scheduled
 * datetime is inside (now - windowMin, now] AND newer than their last-fired
 * epoch. Due entries are marked fired BEFORE returning (crash-safe: a feed
 * failure is the retry journal's problem, not the catch-up scanner's).
 * @param windowMin 0 disables the scan entirely.
 * @return number of due entries written to @p out (up to @p maxOut).
 */
int scanCatchup(const FeedTime *entries, int count, uint32_t nowEpoch,
                uint8_t windowMin, CatchupDue *out, int maxOut);

/** Record that an entry identity fired (persisted, feeds catch-up logic). */
void markFired(uint8_t hour, uint8_t minute, uint32_t epoch);

/** Drop fired-map identities no longer in the schedule (after edits). */
void pruneFired(const FeedTime *entries, int count);

// ------------------------------------------------------ C3: counters
/** Update daily + lifetime counters after every finished feed attempt. */
void onFeedResult(bool ok, uint8_t seconds);

uint16_t feedsOkToday();
uint16_t feedsFailToday();
uint32_t totalFeeds();
uint32_t totalSeconds();
/** Local day stamp the counters refer to, "YYYYMMDD". */
uint32_t statsDay();

// ------------------------------------------- F3/A5: policy + vacation
struct GuardResult {
  bool allowed = true;
  const char *reason = ""; // "vacation" / "min gap" / "daily max" when blocked
};

/**
 * Policy gate for a feed request. On-device touch feeds always pass;
 * scheduled / catch-up / remote feeds are subject to vacation, min-gap and
 * the daily cap.
 */
GuardResult checkFeed(FeedSource src, uint32_t nowEpoch);

// ------------------------------------------------------ D1: hopper
/**
 * Accumulate usage: per-tank applied seconds for every set bit in @p mask.
 * @param perTankSecs index t-1 = seconds applied to tank t (0 = skip).
 */
void hopperAddUsage(uint8_t tankMask, const uint8_t perTankSecs[4]);

/** Reset usage for one tank (1-based); 0xFF refills every configured tank. */
void hopperRefill(uint8_t tank1);

/** 0..100, or 255 when the tank has no capacity configured. */
uint8_t hopperLevelPct(uint8_t tank1);

/** Used pulse-seconds since the last refill for a tank. */
uint16_t hopperUsedSec(uint8_t tank1);

/** true when any CONFIGURED tank has a KNOWN level below the threshold. */
bool hopperLowAny(uint8_t tankCount);

// ------------------------------------------------------ C1: history ring
/** Append one event (oldest entries fall off after FEED_STATE_HIST_MAX). */
void recordHistory(uint8_t tankMask, uint8_t seconds, bool ok, uint8_t flag);

int historyCount();
/** Pointer to the internal ring (index 0 = newest). Read-only. */
const HistEntry *historyAll();

/** Dirty flag consumed by the web snapshot / UI push paths. */
bool historyDirty();
void historyClearDirty();

/** Newest-first JSON array: [{"e":epoch,"m":mask,"s":sec,"ok":0/1,"f":flag}]. */
String historyJson(int maxEntries);

} // namespace feed_state

#endif // CATFISH_FEED_STATE_H
