/**
 * @file feed_scheduler.h
 * @brief Feeding schedule engine for the Catfish Feeder display unit.
 *
 * Up to 8 daily feed times, persisted as a JSON array string in NVS
 * (via prefs). tick() is called ~1 Hz from loop() and returns the entry
 * due at the current (NTP-synced) local HH:MM, firing each entry at most
 * once per calendar minute.
 */
#ifndef CATFISH_FEED_SCHEDULER_H
#define CATFISH_FEED_SCHEDULER_H

#include <Arduino.h>
#include <stdint.h>

struct FeedTime {
  uint8_t hour;    // 0..23
  uint8_t minute;  // 0..59
  bool enabled;    // entry participates in scheduling when true
  uint8_t seconds; // feed pulse duration 1..30 s
};

namespace feed_scheduler {

constexpr int MAX_FEED_TIMES = 8;

/** Load schedule from prefs; seeds defaults (07:30 + 17:30, 3 s) on first boot. */
bool init();

int getCount();

/** Pointer to the internal array (read-only, valid until next mutation). */
const FeedTime *getAll();

bool getEntry(int index, FeedTime &out);

/** Append an entry (clamped/validated). Returns false when full. Persists. */
bool add(const FeedTime &ft);

/** Remove entry at index (shifts the rest). Persists. */
bool remove(int index);

/** Enable/disable an entry. Persists. */
bool setEnabled(int index, bool enabled);

/** Flip an entry's enabled flag. Persists. */
bool toggle(int index);

/** true when local time looks NTP-synced (struct tm year >= 2025). */
bool timeValid();

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
