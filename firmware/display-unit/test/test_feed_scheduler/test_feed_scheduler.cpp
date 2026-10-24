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
  feed_scheduler::init(); // empty blob -> seeds 07:30 + 17:30 (3 s)
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
  TEST_ASSERT_EQUAL_UINT8(1, out.seconds);
}

void test_add_rejects_duplicate_hhmm(void) {
  loadSchedule("[]");
  TEST_ASSERT_TRUE(feed_scheduler::add(FeedTime{7, 30, true, 3}));
  TEST_ASSERT_FALSE(feed_scheduler::add(FeedTime{7, 30, true, 3}));
  // same HH:MM with different seconds is still a duplicate
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
  loadSchedule(R"([{"h":7,"m":30,"e":true,"s":3}])");

  g_now = makeLocalTime(2025, 6, 15, 7, 30, 0);
  const FeedTime due = feed_scheduler::tick();
  TEST_ASSERT_TRUE(due.enabled);
  TEST_ASSERT_EQUAL_UINT8(7, due.hour);
  TEST_ASSERT_EQUAL_UINT8(30, due.minute);
  TEST_ASSERT_EQUAL_UINT8(3, due.seconds);

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
  loadSchedule(R"([{"h":7,"m":30,"e":true,"s":3}])");
  g_now = makeLocalTime(2025, 6, 15, 7, 30, 59);
  TEST_ASSERT_TRUE(feed_scheduler::tick().enabled);
}

void test_tick_skips_disabled_entries(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":false,"s":3}])");
  g_now = makeLocalTime(2025, 6, 15, 7, 30, 10);
  TEST_ASSERT_FALSE(feed_scheduler::tick().enabled);
}

void test_tick_ignores_unsynced_clock(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"s":3}])");
  g_now = makeLocalTime(1970, 1, 1, 7, 30, 0); // pre-NTP pseudo time
  TEST_ASSERT_FALSE(feed_scheduler::timeValid());
  TEST_ASSERT_FALSE(feed_scheduler::tick().enabled);
}

// ---------------------------------------------------------------------------
// nextOccurrence(): today/tomorrow selection + seconds carried
// ---------------------------------------------------------------------------
void test_next_occurrence_today(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"s":3},{"h":17,"m":30,"e":true,"s":5}])");
  g_now = makeLocalTime(2025, 6, 15, 8, 0, 0);
  const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
  TEST_ASSERT_TRUE(nf.valid);
  TEST_ASSERT_FALSE(nf.tomorrow);
  TEST_ASSERT_EQUAL_UINT8(17, nf.hour);
  TEST_ASSERT_EQUAL_UINT8(30, nf.minute);
  TEST_ASSERT_EQUAL_UINT8(5, nf.seconds); // real seconds, not 0
}

void test_next_occurrence_tomorrow_after_last(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"s":3},{"h":17,"m":30,"e":true,"s":5}])");
  g_now = makeLocalTime(2025, 6, 15, 18, 0, 0);
  const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
  TEST_ASSERT_TRUE(nf.valid);
  TEST_ASSERT_TRUE(nf.tomorrow);
  TEST_ASSERT_EQUAL_UINT8(7, nf.hour);
  TEST_ASSERT_EQUAL_UINT8(30, nf.minute);
  TEST_ASSERT_EQUAL_UINT8(3, nf.seconds);
}

void test_next_occurrence_during_firing_minute_shows_tomorrow(void) {
  // Documented quirk: an entry at the current minute is not "today" anymore
  // (strictly-greater selection), so the countdown flips to tomorrow for the
  // remainder of that minute.
  loadSchedule(R"([{"h":7,"m":30,"e":true,"s":3}])");
  g_now = makeLocalTime(2025, 6, 15, 7, 30, 15);
  const feed_scheduler::NextFeed nf = feed_scheduler::nextOccurrence();
  TEST_ASSERT_TRUE(nf.valid);
  TEST_ASSERT_TRUE(nf.tomorrow);
}

// ---------------------------------------------------------------------------
// removeByIdentity()
// ---------------------------------------------------------------------------
void test_remove_by_identity(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"s":3},{"h":17,"m":30,"e":true,"s":5}])");

  TEST_ASSERT_TRUE(feed_scheduler::removeByIdentity(7, 30, 3));
  TEST_ASSERT_EQUAL_INT(1, feed_scheduler::getCount());
  FeedTime out{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, out));
  TEST_ASSERT_EQUAL_UINT8(17, out.hour);

  TEST_ASSERT_FALSE(feed_scheduler::removeByIdentity(9, 9, 9));
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
  TEST_ASSERT_EQUAL_UINT8(4, out.seconds);
}

void test_toggle_marks_dirty_and_stops_firing(void) {
  loadSchedule(R"([{"h":7,"m":30,"e":true,"s":3}])");
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
      R"([{"h":6,"m":15,"e":true,"s":5},{"h":18,"m":45,"e":false,"s":10}])"));
  TEST_ASSERT_EQUAL_INT(2, feed_scheduler::getCount());
  FeedTime e0{}, e1{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, e0));
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(1, e1));
  TEST_ASSERT_EQUAL_UINT8(6, e0.hour);
  TEST_ASSERT_EQUAL_UINT8(5, e0.seconds);
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
      R"([{"h":7,"m":30,"s":3},{"h":7,"m":30,"s":9}])"));
  TEST_ASSERT_EQUAL_INT(2, feed_scheduler::getCount());
  FeedTime e0{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, e0));
  TEST_ASSERT_EQUAL_UINT8(7, e0.hour); // untouched
}

void test_replace_all_rejects_oversize_and_keeps_state(void) {
  String big = "[";
  for (int i = 0; i < feed_scheduler::MAX_FEED_TIMES + 1; i++) {
    big += "{\"h\":23,\"m\":59,\"s\":1}";
    if (i < feed_scheduler::MAX_FEED_TIMES) big += ",";
  }
  big += "]";
  // same HH:MM duplicates would also trip; use distinct minutes
  String ok = "[";
  for (int i = 0; i <= feed_scheduler::MAX_FEED_TIMES; i++) {
    ok += "{\"h\":23,\"m\":" + std::to_string(i) + ",\"s\":1}";
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
      R"([{"h":25,"m":70,"s":99}])"));
  FeedTime e0{};
  TEST_ASSERT_TRUE(feed_scheduler::getEntry(0, e0));
  TEST_ASSERT_EQUAL_UINT8(23, e0.hour); // clamped like add()
  TEST_ASSERT_EQUAL_UINT8(59, e0.minute);
  TEST_ASSERT_EQUAL_UINT8(30, e0.seconds);
}

// ---------------------------------------------------------------------------
// Tank masks (Phase 5)
// ---------------------------------------------------------------------------
void test_legacy_blob_defaults_to_tank1(void) {
  // entries persisted before Phase 5 have no "t" key -> tank 1
  loadSchedule(R"([{"h":7,"m":30,"e":true,"s":3}])");
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
      R"([{"h":6,"m":15,"e":true,"s":5,"t":3},{"h":18,"m":45,"e":true,"s":5}])"));
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
  loadSchedule(R"([{"h":7,"m":30,"e":true,"s":3,"t":5}])");
  g_now = makeLocalTime(2025, 6, 15, 7, 30, 0);
  const FeedTime due = feed_scheduler::tick();
  TEST_ASSERT_TRUE(due.enabled);
  TEST_ASSERT_EQUAL_UINT8(0x05, due.tanks);
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
  return UNITY_END();
}
