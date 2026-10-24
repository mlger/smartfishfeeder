/**
 * @file feed_scheduler.h
 * @brief Feeding schedule engine for the Catfish Feeder display unit.
 *
 * Up to 8 daily feed times, persisted as a JSON array string in NVS
 * (via prefs). tick() is called ~1 Hz from loop() and returns the entry
 * due at the current (NTP-synced) local HH:MM, firing each entry at most
 * once per calendar minute.
 *
 * Persistence is DEFERRED: mutations only mark a dirty flag; loop() calls
 * flushIfDirty() ~1 Hz to serialize + write NVS. This keeps flash writes out
 * of UI callbacks.
 *
 * Threading: ALL mutations (add/remove/setEnabled/toggle) must be called from
 * the main loop() context (single writer). UI callbacks never mutate — they
 * enqueue a UiEvent and loop() applies the change. Reads (tick/nextOccurrence)
 * also run in loop() context, so no locking is needed.
 */
#ifndef CATFISH_FEED_SCHEDULER_H
#define CATFISH_FEED_SCHEDULER_H

#include <Arduino.h>
#include <stdint.h>
#include <time.h>

struct FeedTime {
  uint8_t hour;    // 0..23
  uint8_t minute;  // 0..59
  bool enabled;    // entry participates in scheduling when true
  uint8_t seconds; // feed pulse duration 1..30 s
  uint8_t tanks;   // P5: target-tank bitmask (bit 0 = tank 1). 0 loads as 1
                   // (legacy blobs) and is clamped back to 1 when stored.
};

/**
 * P5: restrict an entry's tank mask to the configured tanks.
 * Pure helper (host-tested): bits beyond tankCount are dropped, and an
 * empty result (e.g. entry for tanks 3-4 while only 1 is configured)
 * falls back to tank 1 so a feed can never address "no tank".
 */
inline uint8_t effectiveTankMask(const FeedTime &e, uint8_t tankCount) {
  if (tankCount > 4) tankCount = 4;
  const uint8_t allowed = (tankCount == 0) ? 0 : (uint8_t)((1u << tankCount) - 1u);
  const uint8_t m = e.tanks & allowed;
  return m ? m : (uint8_t)0x01;
}

namespace feed_scheduler {

constexpr int MAX_FEED_TIMES = 8;

/** Load schedule from prefs; seeds defaults (07:30 + 17:30, 3 s) on first boot. */
bool init();

int getCount();

/** Pointer to the internal array (read-only, valid until next mutation). */
const FeedTime *getAll();

bool getEntry(int index, FeedTime &out);

/**
 * Append an entry (clamped/validated).
 * @return false when full OR an entry with the same HH:MM already exists.
 */
bool add(const FeedTime &ft);

/** Remove entry at index (shifts the rest). Marks persistence dirty. */
bool remove(int index);

/** Remove the first entry matching h:m:s (identity-safe vs index shifts).
 *  @return false when not found. */
bool removeByIdentity(uint8_t hour, uint8_t minute, uint8_t seconds);

/** Enable/disable an entry. Marks persistence dirty. */
bool setEnabled(int index, bool enabled);

/** Flip an entry's enabled flag. Marks persistence dirty. */
bool toggle(int index);

/**
 * Atomically replace the WHOLE schedule from a JSON array (web dashboard
 * PUT /api/schedule). All-or-nothing: on any validation failure (bad JSON,
 * more than MAX_FEED_TIMES entries, duplicate HH:MM inside the payload) the
 * current schedule is left untouched and false is returned. Entries are
 * clamped like add(). Marks persistence dirty.
 * Called from loop() only (single writer).
 */
bool replaceAll(const String &json);

/** true when local time looks NTP-synced (struct tm year >= 2025). */
bool timeValid();

/**
 * Test hook: replace the wall-clock source (nullptr restores system time).
 * Used by the native (host) unit tests; production code never calls it.
 */
void set_time_source(time_t (*fn)());

/**
 * Serialize the schedule and write it to NVS when dirty.
 * Call ~1 Hz from loop().
 * @return true when a flush actually happened.
 */
bool flushIfDirty();

/**
 * Call ~every second from loop().
 * @return the due FeedTime (enabled == true) or a zeroed struct
 *         (enabled == false) when nothing is due.
 */
FeedTime tick();

struct NextFeed {
  bool valid;      // false when time not synced or no enabled entries
  bool tomorrow;   // next occurrence is tomorrow (no more today)
  uint8_t hour;
  uint8_t minute;
  uint8_t seconds;
};

/** Next enabled feeding (today or tomorrow) for UI display. */
NextFeed nextOccurrence();

} // namespace feed_scheduler

#endif // CATFISH_FEED_SCHEDULER_H
