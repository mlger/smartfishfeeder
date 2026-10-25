/**
 * @file test_feed_scheduler.cpp
 * @brief Host-side unit tests for the feeding schedule engine (native env).
 *
 * Runs with:  pio test -d firmware/display-unit -e native
 *
 * The test compiles the real feed_scheduler.cpp via #include (the native env
 * excludes src/ with src_filter) against a mock Arduino.h and two mock prefs
 * functions. Time is fully deterministic via feed_scheduler::set_time_source.
 */
#include <unity.h>

#include <string>

// Pre-include everything feed_scheduler.cpp pulls in BEFORE the compat macro
// below; include guards / #pragma once turn the .cpp's own includes into
// no-ops, so the macro can only ever touch feed_scheduler.cpp's own tokens.
#include <stdint.h>
#include <time.h>
#include <Arduino.h>
#include <catfish_protocol.h>
#include "../../src/feed_scheduler.h" // NOLINT (deliberate relative include)
#include "../../src/prefs.h"          // NOLINT (deliberate relative include)
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// v1.10.0: src/feed_scheduler.cpp compiles clean against the grams API
// (NextFeed::grams); the former test-side `#define seconds grams` shim was
// removed once the src one-liner landed.
// ---------------------------------------------------------------------------
#include "../../src/feed_scheduler.cpp" // NOLINT (deliberate single-TU build)

// ---------------------------------------------------------------------------
// prefs mock — the scheduler only uses these two functions; the real prefs.h
// declarations are satisfied by these definitions at link time.
// ---------------------------------------------------------------------------
namespace prefs {
static std::string g_sched;

static void resetMock() { g_sched.clear(); }
String getScheduleJson() { return g_sched; }
bool setScheduleJson(const String &json) {
  g_sched = json;
  return true;
}
} // namespace prefs

// ---------------------------------------------------------------------------
// Deterministic clock
// ---------------------------------------------------------------------------
static time_t g_now = 0;
static time_t fakeNow() { return g_now; }

/** Build a time_t for the given LOCAL wall time (host timezone — consistent
 *  with the localtime_r() the scheduler uses internally). */
static time_t makeLocalTime(int year, int mon, int mday, int hour, int min,
                            int sec) {
  struct tm lt = {};
  lt.tm_year = year - 1900;
  lt.tm_mon = mon - 1;
  lt.tm_mday = mday;
  lt.tm_hour = hour;
  lt.tm_min = min;
  lt.tm_sec = sec;
  lt.tm_isdst = -1;
  return mktime(&lt);
}

/** Wipe state and load a schedule JSON blob (empty string -> seed defaults). */
static void loadSchedule(const char *json) {
  prefs::resetMock();
  prefs::setScheduleJson(std::string(json));
  feed_scheduler::init();
}

void setUp(void) {
  prefs::resetMock();
  feed_scheduler::set_time_source(fakeNow);
  g_now = makeLocalTime(2025, 6, 15, 12, 0, 0);
  feed_scheduler::init(); // empty blob -> seeds 07:30 + 17:30 (15 g)
}

void tearDown(void) { feed_scheduler::set_time_source(nullptr); }

// ---------------------------------------------------------------------------
// add(): clamping, duplicates, capacity
// ---------------------------------------------------------------------------
void test_add_clamps_out_of_range_entry(void) {
  loadSchedule("[]");
  TEST_ASSERT_TRUE(feed_scheduler::add(FeedTime{25, 70, true, 0}));
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(23, out.hour);
  TEST_ASSERT_EQUAL_UINT8(59, out.minute);
  TEST_ASSERT_EQUAL_UINT16(1, out.grams); // 0 g clamps to 1 (catfishClampGrams)
}

void test_add_rejects_duplicate_hhmm(void) {
  loadSchedule("[]");
  TEST_ASSERT_TRUE(feed_scheduler::add(FeedTime{7, 30, true, 3}));
  TEST_ASSERT_FALSE(feed_scheduler::add(FeedTime{7, 30, true, 3}));
  // same HH:MM with a different portion is still a duplicate
  TEST_ASSERT_FALSE(feed_scheduler::add(FeedTime{7, 30, true, 10}));
  TEST_ASSERT_EQUAL_INT(1, feed_scheduler::getCount());
}

void test_add_enforces_max_entries(void) {
  loadSchedule("[]");
  for (int i = 0; i < feed_scheduler::MAX_FEED_TIMES; i++) {
    TEST_ASSERT_TRUE(feed_scheduler::add(FeedTime{(uint8_t)i, 0, true, 3}));
  }
  TEST_ASSERT_EQUAL_INT(feed_scheduler::MAX_FEED_TIMES,
                        feed_scheduler::getCount());
  TEST_ASSERT_FALSE(feed_scheduler::add(FeedTime{23, 59, true, 3}));
}

// ---------------------------------------------------------------------------
// tick(): firing rules
// ---------------------------------------------------------------------------
void test_tick_fires_once_per_minute(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3}])");

  g_now = makeLocalTime(2025, 6, 15, 7, 30, 0);
  const FeedTime due = feed_scheduler::tick();
  TEST_ASSERT_TRUE(due.enabled);
  TEST_ASSERT_EQUAL_UINT8(7, due.hour);
  TEST_ASSERT_EQUAL_UINT8(30, due.minute);
  TEST_ASSERT_EQUAL_UINT16(3, due.grams); // v1.10.0: the fired portion

  // later in the SAME minute: no refire
  g_now = makeLocalTime(2025, 6, 15, 7, 30, 45);
  TEST_ASSERT_FALSE(feed_scheduler::tick().enabled);

  // next minute: no
  g_now = makeLocalTime(2025, 6, 15, 7, 31, 0);
  TEST_ASSERT_FALSE(feed_scheduler::tick().enabled);
}

void test_tick_fires_on_boot_inside_scheduled_minute(void) {
  // Documented behaviour: a fresh boot inside the scheduled minute fires once
  // (fire tracking starts cold, 0xFFFF). The fish still get fed after a
  // power cut that lands on the feeding minute.
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3}])");
  g_now = makeLocalTime(2025, 6, 15, 7, 30, 59);
  TEST_ASSERT_TRUE(feed_scheduler::tick().enabled);
}

void test_tick_skips_disabled_entries(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":false,"g":3}])");
  g_now = makeLocalTime(2025, 6, 15, 7, 30, 10);
  TEST_ASSERT_FALSE(feed_scheduler::tick().enabled);
}

void test_tick_ignores_unsynced_clock(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3}])");
  g_now = makeLocalTime(1970, 1, 1, 7, 30, 0); // pre-NTP pseudo time
  TEST_ASSERT_FALSE(feed_scheduler::timeValid());
  TEST_ASSERT_FALSE(feed_scheduler::tick().enabled);
}

// ---------------------------------------------------------------------------
// nextOccurrence(): today/tomorrow selection + portion (grams) carried
// ---------------------------------------------------------------------------
void test_next_occurrence_today(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3},{"h":17,"m":30,"e":true,"g":5}])");
  g_now = makeLocalTime(2025, 6, 15, 8, 0, 0);
  const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
  TEST_ASSERT_TRUE(nf.valid);
  TEST_ASSERT_FALSE(nf.tomorrow);
  TEST_ASSERT_EQUAL_UINT8(17, nf.hour);
  TEST_ASSERT_EQUAL_UINT8(30, nf.minute);
  TEST_ASSERT_EQUAL_UINT16(5, nf.grams); // portion carried, not 0
}

void test_next_occurrence_tomorrow_after_last(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3},{"h":17,"m":30,"e":true,"g":5}])");
  g_now = makeLocalTime(2025, 6, 15, 18, 0, 0);
  const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
  TEST_ASSERT_TRUE(nf.valid);
  TEST_ASSERT_TRUE(nf.tomorrow);
  TEST_ASSERT_EQUAL_UINT8(7, nf.hour);
  TEST_ASSERT_EQUAL_UINT8(30, nf.minute);
  TEST_ASSERT_EQUAL_UINT16(3, nf.grams);
}

void test_next_occurrence_during_firing_minute_shows_tomorrow(void) {
  // Documented quirk: an entry at the current minute is not "today" anymore
  // (strictly-greater selection), so the countdown flips to tomorrow for the
  // remainder of that minute.
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3}])");
  g_now = makeLocalTime(2025, 6, 15, 7, 30, 15);
  const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
  TEST_ASSERT_TRUE(nf.valid);
  TEST_ASSERT_TRUE(nf.tomorrow);
}

// ---------------------------------------------------------------------------
// removeByIdentity()
// ---------------------------------------------------------------------------
void test_remove_by_identity(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3},{"h":17,"m":30,"e":true,"g":5}])");

  TEST_ASSERT_TRUE(feed_scheduler::removeByIdentity(7, 30));
  TEST_ASSERT_EQUAL_INT(1, feed_scheduler::getCount());
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(17, out.hour);

  TEST_ASSERT_FALSE(feed_scheduler::removeByIdentity(9, 9));
}

// v1.8.2 regression: the UI queues enable toggles by identity (the list can
// change while the event sits in the queue). After a dashboard replaceAll
// that SHIFTS indices, an identity-based toggle must still hit the right
// entry — the index-based version flipped the wrong one.
void test_set_enabled_by_identity_survives_index_shift(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3},{"h":8,"m":0,"e":true,"g":4},{"h":17,"m":30,"e":true,"g":5}])");

  // toggle the 08:00 entry by identity, then simulate the queued event
  // landing AFTER a dashboard replace removed the 07:30 entry (index 1 -> 0)
  TEST_ASSERT_TRUE(feed_scheduler::removeByIdentity(7, 30));
  TEST_ASSERT_TRUE(feed_scheduler::setEnabledByIdentity(8, 0, false));
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(8, out.hour);
  TEST_ASSERT_FALSE(out.enabled);            // the TARGET entry was toggled
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(1, out));
  TEST_ASSERT_EQUAL_UINT8(17, out.hour);
  TEST_ASSERT_TRUE(out.enabled);             // neighbors untouched

  TEST_ASSERT_FALSE(feed_scheduler::setEnabledByIdentity(9, 9, true));
}

// ---------------------------------------------------------------------------
// Persistence: dirty flag + JSON round-trip
// ---------------------------------------------------------------------------
void test_persistence_flush_and_roundtrip(void) {
  loadSchedule("[]");
  TEST_ASSERT_TRUE(feed_scheduler::add(FeedTime{6, 15, true, 4}));

  TEST_ASSERT_TRUE(feed_scheduler::flushIfDirty());  // dirty -> writes
  TEST_ASSERT_FALSE(feed_scheduler::flushIfDirty()); // clean -> no-op
  TEST_ASSERT_NOT_NULL(strstr(prefs::g_sched.c_str(), "\"h\":6"));

  // reload the exact blob the "NVS" now holds and verify it round-trips
  const std::string blob = prefs::g_sched;
  loadSchedule(blob.c_str());
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(6, out.hour);
  TEST_ASSERT_EQUAL_UINT8(15, out.minute);
  TEST_ASSERT_EQUAL_UINT16(4, out.grams);
}

void test_toggle_marks_dirty_and_stops_firing(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3}])");
  TEST_ASSERT_TRUE(feed_scheduler::toggle(0));
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_FALSE(out.enabled);

  g_now = makeLocalTime(2025, 6, 15, 7, 30, 5);
  TEST_ASSERT_FALSE(feed_scheduler::tick().enabled);
  TEST_ASSERT_TRUE(feed_scheduler::flushIfDirty()); // mutation was persisted
}

// ---------------------------------------------------------------------------
// replaceAll(): atomic whole-schedule replace (web dashboard PUT)
// ---------------------------------------------------------------------------
void test_replace_all_swaps_everything(void) {
  TEST_ASSERT_TRUE(feed_scheduler::replaceAll(
      R"([{"h":6,"m":15,"e":true,"g":5},{"h":18,"m":45,"e":false,"g":10}])"));
  TEST_ASSERT_EQUAL_INT(2, feed_scheduler::getCount());
  FeedTime e0{}, e1{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, e0));
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(1, e1));
  TEST_ASSERT_EQUAL_UINT8(6, e0.hour);
  TEST_ASSERT_EQUAL_UINT16(5, e0.grams);
  TEST_ASSERT_EQUAL_UINT8(18, e1.hour);
  TEST_ASSERT_FALSE(e1.enabled);
  TEST_ASSERT_TRUE(feed_scheduler::flushIfDirty()); // replacement persisted
}

void test_replace_all_rejects_bad_json_and_keeps_state(void) {
  TEST_ASSERT_FALSE(feed_scheduler::replaceAll("not json"));
  TEST_ASSERT_FALSE(feed_scheduler::replaceAll("{\"h\":1}")); // not an array
  TEST_ASSERT_EQUAL_INT(2, feed_scheduler::getCount()); // setUp defaults intact
}

void test_replace_all_rejects_duplicates_and_keeps_state(void) {
  TEST_ASSERT_FALSE(feed_scheduler::replaceAll(
      R"([{"h":7,"m":30,"g":3},{"h":7,"m":30,"g":9}])"));
  TEST_ASSERT_EQUAL_INT(2, feed_scheduler::getCount());
  FeedTime e0{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, e0));
  TEST_ASSERT_EQUAL_UINT8(7, e0.hour); // untouched
}

void test_replace_all_rejects_oversize_and_keeps_state(void) {
  String big = "[";
  for (int i = 0; i < feed_scheduler::MAX_FEED_TIMES + 1; i++) {
    big += "{\"h\":23,\"m\":59,\"g\":1}";
    if (i < feed_scheduler::MAX_FEED_TIMES) big += ",";
  }
  big += "]";
  // same HH:MM duplicates would also trip; use distinct minutes
  String ok = "[";
  for (int i = 0; i <= feed_scheduler::MAX_FEED_TIMES; i++) {
    ok += "{\"h\":23,\"m\":" + std::to_string(i) + ",\"g\":1}";
    if (i < feed_scheduler::MAX_FEED_TIMES) ok += ",";
  }
  ok += "]";
  TEST_ASSERT_FALSE(feed_scheduler::replaceAll(ok));
  TEST_ASSERT_EQUAL_INT(2, feed_scheduler::getCount());
}

void test_replace_all_accepts_empty_array_and_clamps(void) {
  TEST_ASSERT_TRUE(feed_scheduler::replaceAll("[]"));
  TEST_ASSERT_EQUAL_INT(0, feed_scheduler::getCount());

  TEST_ASSERT_TRUE(feed_scheduler::replaceAll(
      R"([{"h":25,"m":70,"g":9999}])"));
  FeedTime e0{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, e0));
  TEST_ASSERT_EQUAL_UINT8(23, e0.hour); // clamped like add()
  TEST_ASSERT_EQUAL_UINT8(59, e0.minute);
  TEST_ASSERT_EQUAL_UINT16(500, e0.grams); // 9999 g -> CATFISH_MAX_GRAMS
}

// ---------------------------------------------------------------------------
// Tank masks (Phase 5)
// ---------------------------------------------------------------------------
void test_legacy_blob_defaults_to_tank1(void) {
  // entries persisted before Phase 5 have no "t" key -> tank 1
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3}])");
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(0x01, out.tanks);
}

void test_add_persists_and_roundtrips_tank_mask(void) {
  loadSchedule("[]");
  TEST_ASSERT_TRUE(feed_scheduler::add(FeedTime{6, 15, true, 4, 0x05}));
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(0x05, out.tanks);

  TEST_ASSERT_TRUE(feed_scheduler::flushIfDirty());
  TEST_ASSERT_NOT_NULL(strstr(prefs::g_sched.c_str(), "\"t\":5"));

  const std::string blob = prefs::g_sched;
  loadSchedule(blob.c_str());
  FeedTime rt{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, rt));
  TEST_ASSERT_EQUAL_UINT8(0x05, rt.tanks);
}

void test_add_clamps_tank_mask(void) {
  loadSchedule("[]");
  TEST_ASSERT_TRUE(feed_scheduler::add(FeedTime{6, 15, true, 4, 0}));
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(0x01, out.tanks); // 0 -> tank 1

  TEST_ASSERT_TRUE(feed_scheduler::add(FeedTime{6, 16, true, 4, 0xF0}));
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(1, out));
  TEST_ASSERT_EQUAL_UINT8(0x01, out.tanks); // 0xF0 & 0x0F == 0 -> tank 1
}

void test_replace_all_parses_tank_mask(void) {
  TEST_ASSERT_TRUE(feed_scheduler::replaceAll(
      R"([{"h":6,"m":15,"e":true,"g":5,"t":3},{"h":18,"m":45,"e":true,"g":5}])"));
  FeedTime e0{}, e1{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, e0));
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(1, e1));
  TEST_ASSERT_EQUAL_UINT8(0x03, e0.tanks); // tanks 1+2
  TEST_ASSERT_EQUAL_UINT8(0x01, e1.tanks); // missing "t" -> tank 1
}

void test_effective_tank_mask_clamps_to_configured(void) {
  FeedTime e{7, 30, true, 3, 0x0F};
  TEST_ASSERT_EQUAL_UINT8(0x0F, effectiveTankMask(e, 4));
  TEST_ASSERT_EQUAL_UINT8(0x03, effectiveTankMask(e, 2));
  TEST_ASSERT_EQUAL_UINT8(0x01, effectiveTankMask(e, 1));
  FeedTime z{7, 30, true, 3, 0};
  TEST_ASSERT_EQUAL_UINT8(0x01, effectiveTankMask(z, 4)); // empty -> tank 1
  FeedTime hi{7, 30, true, 3, 0xF0};
  TEST_ASSERT_EQUAL_UINT8(0x01, effectiveTankMask(hi, 4)); // bits dropped -> tank 1
  // out-of-range counts are clamped defensively
  TEST_ASSERT_EQUAL_UINT8(0x0F, effectiveTankMask(e, 9));
  TEST_ASSERT_EQUAL_UINT8(0x01, effectiveTankMask(e, 0));
}

void test_tick_preserves_tank_mask(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3,"t":5}])");
  g_now = makeLocalTime(2025, 6, 15, 7, 30, 0);
  const FeedTime due = feed_scheduler::tick();
  TEST_ASSERT_TRUE(due.enabled);
  TEST_ASSERT_EQUAL_UINT8(0x05, due.tanks);
}

// ---------------------------------------------------------------------------
// v1.9.0: weekday masks (calendar used below: 2025-06-15 = Sunday,
// 2025-06-16 = Monday, 2025-06-17 = Tuesday, 2025-06-18 = Wednesday)
// ---------------------------------------------------------------------------
void test_entry_covers_wday_bit_mapping(void) {
  FeedTime mon{7, 30, true, 3, 0x01};
  mon.days = 0x01; // bit0 = Monday
  TEST_ASSERT_TRUE(entryCoversWday(mon, 1));  // tm_wday 1 = Monday
  TEST_ASSERT_FALSE(entryCoversWday(mon, 0)); // Sunday
  TEST_ASSERT_FALSE(entryCoversWday(mon, 6)); // Saturday

  FeedTime all{7, 30, true, 3}; // default member init = 0x7F
  TEST_ASSERT_EQUAL_UINT8(0x7F, all.days);
  for (int w = 0; w <= 6; w++) TEST_ASSERT_TRUE(entryCoversWday(all, w));

  FeedTime sun{7, 30, true, 3};
  sun.days = 0x40; // bit6 = Sunday
  TEST_ASSERT_TRUE(entryCoversWday(sun, 0));
  TEST_ASSERT_FALSE(entryCoversWday(sun, 1));
}

void test_tick_weekday_mask_monday_only(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3,"d":1}])");
  g_now = makeLocalTime(2025, 6, 16, 7, 30, 0); // Monday
  const FeedTime due = feed_scheduler::tick();
  TEST_ASSERT_TRUE(due.enabled);
  TEST_ASSERT_EQUAL_UINT8(7, due.hour);
  TEST_ASSERT_EQUAL_UINT8(0x01, due.days);

  // same wall time on Tuesday: the mask keeps it silent
  g_now = makeLocalTime(2025, 6, 17, 7, 30, 0); // Tuesday
  TEST_ASSERT_FALSE(feed_scheduler::tick().enabled);
}

void test_tick_default_mask_fires_every_day(void) {
  // no "d" key -> legacy blob -> every-day (0x7F)
  const char *json = R"([{"h":7,"m":30,"e":true,"g":3}])";
  loadSchedule(json);
  g_now = makeLocalTime(2025, 6, 17, 7, 30, 0); // Tuesday
  TEST_ASSERT_TRUE(feed_scheduler::tick().enabled);

  // A reload (== reboot) clears fire tracking, so the next calendar day
  // fires again at the same HH:MM. NOTE: within ONE process the fire
  // tracker is minute-of-day only and does NOT re-fire on consecutive
  // days (no midnight rollover in feed_scheduler.cpp) — reported as a
  // v1.9.0 finding in the worklog, deliberately not asserted here.
  loadSchedule(json);
  g_now = makeLocalTime(2025, 6, 18, 7, 30, 0); // Wednesday
  TEST_ASSERT_TRUE(feed_scheduler::tick().enabled);
}

// ---------------------------------------------------------------------------
// v1.10.0: clamping (days 0 -> 0x7F, gramTank > 500 -> 500)
// ---------------------------------------------------------------------------
void test_days_zero_clamps_to_daily(void) {
  // stored blob with an empty mask
  loadSchedule(R"([{"h":8,"m":0,"e":true,"g":3,"d":0}])");
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(0x7F, out.days);

  // add() with days = 0 (user cleared every checkbox)
  loadSchedule("[]");
  FeedTime t{8, 1, true, 3, 0x01};
  t.days = 0;
  TEST_ASSERT_TRUE(feed_scheduler::add(t));
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(0x7F, out.days);

  // and the clamped value is what gets persisted
  TEST_ASSERT_TRUE(feed_scheduler::flushIfDirty());
  TEST_ASSERT_NOT_NULL(strstr(prefs::g_sched.c_str(), "\"d\":127"));
}

void test_gramtank_clamps_over_max(void) {
  loadSchedule(R"([{"h":8,"m":0,"e":true,"g":3,"gt":[600,0,0,0]}])");
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT16(500, out.gramTank[0]); // 600 -> CATFISH_MAX_GRAMS
  TEST_ASSERT_EQUAL_UINT16(0, out.gramTank[1]);   // 0 = inherit shared
  TEST_ASSERT_EQUAL_UINT16(3, out.grams);

  loadSchedule("[]");
  FeedTime t{8, 1, true, 5, 0x0F};
  t.gramTank[0] = 600;
  t.gramTank[3] = 501;
  TEST_ASSERT_TRUE(feed_scheduler::add(t));
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT16(500, out.gramTank[0]);
  TEST_ASSERT_EQUAL_UINT16(500, out.gramTank[3]);
}

void test_entry_grams_for_tank_fallback_and_override(void) {
  FeedTime e{7, 30, true, 5, 0x0F}; // shared 5 g, no per-tank overrides
  TEST_ASSERT_EQUAL_UINT16(5, entryGramsForTank(e, 1));
  TEST_ASSERT_EQUAL_UINT16(5, entryGramsForTank(e, 4));

  e.gramTank[0] = 12;
  e.gramTank[1] = 8;
  TEST_ASSERT_EQUAL_UINT16(12, entryGramsForTank(e, 1)); // override tank 1
  TEST_ASSERT_EQUAL_UINT16(8, entryGramsForTank(e, 2));  // override tank 2
  TEST_ASSERT_EQUAL_UINT16(5, entryGramsForTank(e, 3));  // 0 -> shared
  TEST_ASSERT_EQUAL_UINT16(5, entryGramsForTank(e, 5));  // out of range -> shared

  // wire path (kept for the relay): grams -> pulse seconds via the tank gps
  TEST_ASSERT_EQUAL_UINT8(3, entrySecondsForTank(e, 1, 5)); // ceil(12/5)
  TEST_ASSERT_EQUAL_UINT8(2, entrySecondsForTank(e, 2, 5)); // ceil(8/5)
  TEST_ASSERT_EQUAL_UINT8(1, entrySecondsForTank(e, 3, 5)); // ceil(5/5)
}

// ---------------------------------------------------------------------------
// v1.10.0: JSON roundtrip (d + gt survive), legacy blobs without d/gt
// ---------------------------------------------------------------------------
void test_v1100_json_roundtrip_days_and_gramtank(void) {
  loadSchedule("[]");
  FeedTime t{6, 15, true, 4, 0x03};
  t.days = 0x02; // Tuesday only
  t.gramTank[0] = 12;
  t.gramTank[1] = 8;
  TEST_ASSERT_TRUE(feed_scheduler::add(t));
  TEST_ASSERT_TRUE(feed_scheduler::flushIfDirty());
  TEST_ASSERT_NOT_NULL(strstr(prefs::g_sched.c_str(), "\"d\":2"));
  TEST_ASSERT_NOT_NULL(strstr(prefs::g_sched.c_str(), "\"gt\":[12,8,0,0]"));

  // reload the exact blob the "NVS" now holds
  const std::string blob = prefs::g_sched;
  loadSchedule(blob.c_str());
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(0x02, out.days);
  TEST_ASSERT_EQUAL_UINT16(12, out.gramTank[0]);
  TEST_ASSERT_EQUAL_UINT16(8, out.gramTank[1]);
  TEST_ASSERT_EQUAL_UINT16(0, out.gramTank[2]);
  TEST_ASSERT_EQUAL_UINT16(4, out.grams);
  TEST_ASSERT_EQUAL_UINT8(0x03, out.tanks);
}

void test_legacy_blob_without_d_gt_loads_daily_and_shared(void) {
  // pre-v1.10.0 blob shape: no "d", no "gt"
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3,"t":5}])");
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(0x7F, out.days); // absent -> every day
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_EQUAL_UINT16(0, out.gramTank[i]); // absent gt -> shared grams
  }
}

// ---------------------------------------------------------------------------
// v1.10.0: legacy "s" blobs migrate to grams at the nominal rate (5 g/s)
// ---------------------------------------------------------------------------
void test_legacy_s_blob_migrates_to_grams(void) {
  // 3 s @ 5 g/s -> 15 g; 10 s -> 50 g; neither "g" nor "s" -> default 3 s
  loadSchedule(R"([{"h":8,"m":0,"e":true,"s":3,"t":1},)"
               R"({"h":9,"m":0,"e":true,"s":10},)"
               R"({"h":10,"m":0,"e":true}])");
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT16(15, out.grams);
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(1, out));
  TEST_ASSERT_EQUAL_UINT16(50, out.grams);
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(2, out));
  TEST_ASSERT_EQUAL_UINT16(15, out.grams);

  // a dashboard replaceAll of a legacy-shaped payload persists GRAMS now
  TEST_ASSERT_TRUE(feed_scheduler::replaceAll(
      R"([{"h":8,"m":0,"e":true,"s":3,"t":1}])"));
  TEST_ASSERT_TRUE(feed_scheduler::flushIfDirty());
  TEST_ASSERT_NOT_NULL(strstr(prefs::g_sched.c_str(), "\"g\":15"));
  TEST_ASSERT_NULL(strstr(prefs::g_sched.c_str(), "\"s\":"));
}

// ---------------------------------------------------------------------------
// v1.10.0: seedDefaults() portions + grams<->seconds conversion helpers
// from catfish_protocol.h (pure, host-testable)
// ---------------------------------------------------------------------------
void test_seed_defaults_grams(void) {
  loadSchedule(""); // empty blob -> first boot -> seedDefaults()
  TEST_ASSERT_EQUAL_INT(2, feed_scheduler::getCount());
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(7, out.hour);
  TEST_ASSERT_EQUAL_UINT8(30, out.minute);
  TEST_ASSERT_EQUAL_UINT16(CATFISH_DEFAULT_GRAMS, out.grams);
  TEST_ASSERT_EQUAL_UINT8(0x01, out.tanks);
  TEST_ASSERT_EQUAL_UINT8(0x7F, out.days);
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(1, out));
  TEST_ASSERT_EQUAL_UINT8(17, out.hour);
  TEST_ASSERT_EQUAL_UINT8(30, out.minute);
  TEST_ASSERT_EQUAL_UINT16(CATFISH_DEFAULT_GRAMS, out.grams);
}

void test_grams_seconds_conversion_helpers(void) {
  // grams -> wire seconds: ceil at the tank's g/s; gps 0 falls back to 5
  TEST_ASSERT_EQUAL_UINT8(3, catfishGramsToSeconds(15, 5));
  TEST_ASSERT_EQUAL_UINT8(3, catfishGramsToSeconds(15, 0)); // default gps
  TEST_ASSERT_EQUAL_UINT8(1, catfishGramsToSeconds(1, 5));
  TEST_ASSERT_EQUAL_UINT8(60, catfishGramsToSeconds(500, 5)); // wire hard cap
  TEST_ASSERT_EQUAL_UINT8(60, catfishGramsToSeconds(9999, 5)); // grams clamp first

  // seconds -> approximate grams, clamped to CATFISH_MAX_GRAMS
  TEST_ASSERT_EQUAL_UINT16(15, catfishSecondsToGrams(3, 5));
  TEST_ASSERT_EQUAL_UINT16(300, catfishSecondsToGrams(60, 5));
  TEST_ASSERT_EQUAL_UINT16(500, catfishSecondsToGrams(255, 5)); // clamp
  TEST_ASSERT_EQUAL_UINT16(25, catfishSecondsToGrams(5, 0)); // default gps

  // portion clamp
  TEST_ASSERT_EQUAL_UINT16(1, catfishClampGrams(0));
  TEST_ASSERT_EQUAL_UINT16(500, catfishClampGrams(9999));
  TEST_ASSERT_EQUAL_UINT16(123, catfishClampGrams(123));
}

// ---------------------------------------------------------------------------
// v1.9.0: nextOccurrence() scans up to 8 days ahead
// ---------------------------------------------------------------------------
void test_next_occurrence_mon_only_from_tuesday(void) {
  // Tuesday 08:00; Mon-only 07:30 -> next Monday is 6 days out
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3,"d":1}])");
  g_now = makeLocalTime(2025, 6, 17, 8, 0, 0); // Tuesday
  const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
  TEST_ASSERT_TRUE(nf.valid);
  TEST_ASSERT_TRUE(nf.tomorrow);
  TEST_ASSERT_EQUAL_UINT8(6, nf.daysAhead);
  TEST_ASSERT_EQUAL_UINT8(7, nf.hour);
  TEST_ASSERT_EQUAL_UINT8(30, nf.minute);
  TEST_ASSERT_EQUAL_UINT16(3, nf.grams);
}

void test_next_occurrence_mon_only_from_sunday_is_tomorrow(void) {
  // Sunday 08:00 (after the 07:30 slot); next Monday = +1 day
  loadSchedule(R"([{"h":7,"m":30,"e":true,"g":3,"d":1}])");
  g_now = makeLocalTime(2025, 6, 15, 8, 0, 0); // Sunday
  const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
  TEST_ASSERT_TRUE(nf.valid);
  TEST_ASSERT_TRUE(nf.tomorrow);
  TEST_ASSERT_EQUAL_UINT8(1, nf.daysAhead);
}

void test_next_occurrence_today_future_days_ahead_zero(void) {
  loadSchedule(R"([{"h":17,"m":30,"e":true,"g":3}])");
  g_now = makeLocalTime(2025, 6, 15, 8, 0, 0); // Sunday morning
  const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
  TEST_ASSERT_TRUE(nf.valid);
  TEST_ASSERT_FALSE(nf.tomorrow);
  TEST_ASSERT_EQUAL_UINT8(0, nf.daysAhead);
}

// ---------------------------------------------------------------------------
// v1.9.0 (E3): hasNearDuplicate() — gap, midnight wrap, disjoint days
// ---------------------------------------------------------------------------
void test_has_near_duplicate_detection(void) {
  FeedTime a{7, 0, true, 3};  // daily 07:00
  FeedTime b{7, 5, true, 3};  // daily 07:05
  FeedTime pair[2] = {a, b};
  TEST_ASSERT_TRUE(hasNearDuplicate(pair, 2, 10));   // 5 < 10
  TEST_ASSERT_FALSE(hasNearDuplicate(pair, 2, 5));   // 5 is not < 5
  TEST_ASSERT_FALSE(hasNearDuplicate(pair, 2, 0));   // gap disabled
  TEST_ASSERT_FALSE(hasNearDuplicate(nullptr, 2, 10));

  // circular distance over midnight: 23:55 + 00:05 -> 10 min apart
  FeedTime n1{23, 55, true, 3};
  FeedTime n2{0, 5, true, 3};
  FeedTime wrap[2] = {n1, n2};
  TEST_ASSERT_TRUE(hasNearDuplicate(wrap, 2, 15));
  TEST_ASSERT_FALSE(hasNearDuplicate(wrap, 2, 10)); // 10 is not < 10

  // disjoint weekday masks never share a day -> no warning
  FeedTime mon{7, 0, true, 3};
  mon.days = 0x01;
  FeedTime tue{7, 5, true, 3};
  tue.days = 0x02;
  FeedTime disjoint[2] = {mon, tue};
  TEST_ASSERT_FALSE(hasNearDuplicate(disjoint, 2, 10));

  // disabled entries are ignored
  FeedTime off{7, 1, false, 3};
  FeedTime withOff[2] = {a, off};
  TEST_ASSERT_FALSE(hasNearDuplicate(withOff, 2, 10));
}

// v1.9.0 regression: the same HH:MM must re-fire on LATER days of one uptime.
// The old minute-of-day fire stamp blocked day-2+ fires entirely (the catch-up
// scan masked it by feeding 0-10 min late); the epoch-minute stamp fixes it.
static void test_tick_refires_same_hhmm_next_day(void) {
  // 2026-01-05 is a Monday; 07:30 entry, two consecutive days, same uptime.
  loadSchedule("[{\"h\":7,\"m\":30,\"e\":true,\"g\":3,\"t\":1,\"d\":127}]");
  g_now = makeLocalTime(2026, 1, 5, 7, 30, 30); // Monday
  FeedTime due = feed_scheduler::tick();
  TEST_ASSERT_TRUE(due.enabled);

  g_now = makeLocalTime(2026, 1, 5, 7, 31, 0);
  due = feed_scheduler::tick();
  TEST_ASSERT_FALSE(due.enabled); // same minute is exhausted

  g_now = makeLocalTime(2026, 1, 6, 7, 30, 30); // Tuesday, NEXT DAY
  due = feed_scheduler::tick();
  TEST_ASSERT_TRUE(due.enabled); // must fire again on day 2

  // midnight wrap: 00:05 entry seen at 00:05 on two consecutive days
  loadSchedule("[{\"h\":0,\"m\":5,\"e\":true,\"g\":3,\"t\":1,\"d\":127}]");
  g_now = makeLocalTime(2026, 1, 6, 0, 5, 30);
  TEST_ASSERT_TRUE(feed_scheduler::tick().enabled);
  g_now = makeLocalTime(2026, 1, 7, 0, 5, 30);
  TEST_ASSERT_TRUE(feed_scheduler::tick().enabled);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_add_clamps_out_of_range_entry);
  RUN_TEST(test_add_rejects_duplicate_hhmm);
  RUN_TEST(test_add_enforces_max_entries);
  RUN_TEST(test_tick_fires_once_per_minute);
  RUN_TEST(test_tick_fires_on_boot_inside_scheduled_minute);
  RUN_TEST(test_tick_skips_disabled_entries);
  RUN_TEST(test_tick_ignores_unsynced_clock);
  RUN_TEST(test_next_occurrence_today);
  RUN_TEST(test_next_occurrence_tomorrow_after_last);
  RUN_TEST(test_next_occurrence_during_firing_minute_shows_tomorrow);
  RUN_TEST(test_remove_by_identity);
  RUN_TEST(test_set_enabled_by_identity_survives_index_shift);
  RUN_TEST(test_persistence_flush_and_roundtrip);
  RUN_TEST(test_toggle_marks_dirty_and_stops_firing);
  RUN_TEST(test_replace_all_swaps_everything);
  RUN_TEST(test_replace_all_rejects_bad_json_and_keeps_state);
  RUN_TEST(test_replace_all_rejects_duplicates_and_keeps_state);
  RUN_TEST(test_replace_all_rejects_oversize_and_keeps_state);
  RUN_TEST(test_replace_all_accepts_empty_array_and_clamps);
  RUN_TEST(test_legacy_blob_defaults_to_tank1);
  RUN_TEST(test_add_persists_and_roundtrips_tank_mask);
  RUN_TEST(test_add_clamps_tank_mask);
  RUN_TEST(test_replace_all_parses_tank_mask);
  RUN_TEST(test_effective_tank_mask_clamps_to_configured);
  RUN_TEST(test_tick_preserves_tank_mask);
  RUN_TEST(test_entry_covers_wday_bit_mapping);
  RUN_TEST(test_tick_weekday_mask_monday_only);
  RUN_TEST(test_tick_default_mask_fires_every_day);
  RUN_TEST(test_days_zero_clamps_to_daily);
  RUN_TEST(test_gramtank_clamps_over_max);
  RUN_TEST(test_entry_grams_for_tank_fallback_and_override);
  RUN_TEST(test_v1100_json_roundtrip_days_and_gramtank);
  RUN_TEST(test_legacy_blob_without_d_gt_loads_daily_and_shared);
  RUN_TEST(test_legacy_s_blob_migrates_to_grams);
  RUN_TEST(test_seed_defaults_grams);
  RUN_TEST(test_grams_seconds_conversion_helpers);
  RUN_TEST(test_next_occurrence_mon_only_from_tuesday);
  RUN_TEST(test_next_occurrence_mon_only_from_sunday_is_tomorrow);
  RUN_TEST(test_next_occurrence_today_future_days_ahead_zero);
  RUN_TEST(test_has_near_duplicate_detection);
  RUN_TEST(test_tick_refires_same_hhmm_next_day);
  return UNITY_END();
}
