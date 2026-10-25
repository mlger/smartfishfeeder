/**
 * @file test_feed_state.cpp
 * @brief Host-side unit tests for the v1.9.0 persistent feeding state module
 *        (native env).
 *
 * Runs with:  pio test -d firmware/display-unit -e native
 *
 * The test compiles the real feed_state.cpp via #include (single TU, same
 * pattern as test_feed_scheduler) against a mock Arduino.h and a mock of the
 * exact prefs subset feed_state touches. Time is fully deterministic via
 * feed_state::set_time_source — every state function takes its epoch IN, the
 * injected clock only feeds journalArm/journalFail/rollStatsIfNeeded/
 * recordHistory.
 *
 * Calendar anchor: 2025-06-16 is a MONDAY (matches the scheduler tests).
 */
#include <unity.h>

#include <string>

#include <ArduinoJson.h>

#include "../../src/feed_state.cpp" // NOLINT (deliberate single-TU build)

// ---------------------------------------------------------------------------
// prefs mock — exactly the subset feed_state.cpp uses (see prefs.h). Getters
// without a firmware setter (lastFed, hopper caps) are driven through the
// exposed mock variables, like prefs::g_sched in the scheduler test.
// ---------------------------------------------------------------------------
namespace prefs {
static std::string g_retry, g_fired, g_stats, g_hopper;
static uint32_t g_skipMin = 0xFFFF;
static bool g_vacation = false;
static uint8_t g_minGap = 0, g_maxFeeds = 0, g_catchupWin = 10, g_lowPct = 20;
static uint32_t g_lastFed = 0;
static uint16_t g_cap[5] = {0, 0, 0, 0, 0}; // hopper caps (g), index 1..4
static uint16_t g_gps[5] = {0, 0, 0, 0, 0}; // per-tank g/s rate, index 1..4

static void resetMock() {
  g_retry.clear();
  g_fired.clear();
  g_stats.clear();
  g_hopper.clear();
  g_skipMin = 0xFFFF;
  g_vacation = false;
  g_minGap = 0;
  g_maxFeeds = 0;
  g_catchupWin = 10;
  g_lowPct = 20;
  g_lastFed = 0;
  for (int i = 0; i < 5; i++) {
    g_cap[i] = 0;
    g_gps[i] = 0;
  }
}

String getRetryJson() { return g_retry; }
bool setRetryJson(const String &j) {
  g_retry = j;
  return true;
}
String getFiredJson() { return g_fired; }
bool setFiredJson(const String &j) {
  g_fired = j;
  return true;
}
String getStatsJson() { return g_stats; }
bool setStatsJson(const String &j) {
  g_stats = j;
  return true;
}
String getHopperJson() { return g_hopper; }
bool setHopperJson(const String &j) {
  g_hopper = j;
  return true;
}
uint32_t getSkipMinute() { return g_skipMin; }
void setSkipMinute(uint32_t m) { g_skipMin = m; }
bool getVacation() { return g_vacation; }
void setVacation(bool on) { g_vacation = on; }
uint8_t getMinGapMin() { return g_minGap; }
void setMinGapMin(uint8_t m) { g_minGap = m; }
uint8_t getMaxFeedsDay() { return g_maxFeeds; }
void setMaxFeedsDay(uint8_t m) { g_maxFeeds = m; }
uint8_t getCatchupWin() { return g_catchupWin; } // getter-only (feed_state takes the window as a parameter)
uint8_t getHopperLowPct() { return g_lowPct; }
uint32_t getLastFedEpoch() { return g_lastFed; }
uint16_t getTankGps(uint8_t tank1) { // v1.10.0: legacy "s" journal migration
  return (tank1 >= 1 && tank1 <= 4) ? g_gps[tank1] : 0;
}
uint16_t getTankHopperCap(uint8_t tank1) {
  return (tank1 >= 1 && tank1 <= 4) ? g_cap[tank1] : 0;
}
} // namespace prefs

// ---------------------------------------------------------------------------
// Deterministic clock
// ---------------------------------------------------------------------------
static time_t g_now = 0;
static time_t fakeNow() { return g_now; }

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

/** Monday 2025-06-16 12:00:00 local — the per-test anchor epoch. */
static uint32_t anchorEpoch() {
  return (uint32_t)makeLocalTime(2025, 6, 16, 12, 0, 0);
}

void setUp(void) {
  prefs::resetMock();
  // Present a "factory fresh" boot: in production the statics zero-init at
  // power-on and the blobs are empty on first boot. Within one host process
  // the statics survive between tests, so seed the stats/hopper blobs with
  // zeroed values for init() to load (a harness artifact, not firmware
  // behaviour — init() is only ever called once per power cycle).
  prefs::g_stats = "{\"d\":20250616,\"ok\":0,\"fail\":0,\"tot\":0,\"sec\":0}";
  prefs::g_hopper = "[0,0,0,0]";
  feed_state::set_time_source(fakeNow);
  g_now = makeLocalTime(2025, 6, 16, 12, 0, 0);
  feed_state::init();
  // init() does not touch the RAM-only history ring; reset it for isolation
  // (in production a reboot clears it via static init).
  s_hist_n = 0;
  s_hist_dirty = false;
}

void tearDown(void) { feed_state::set_time_source(nullptr); }

// ---------------------------------------------------------------------------
// B1: retry journal
// ---------------------------------------------------------------------------
void test_journal_due_gates_on_first_backoff(void) {
  TEST_ASSERT_TRUE(feed_state::journalArm(7, 30, 3, 0x01));
  TEST_ASSERT_EQUAL_INT(1, feed_state::journalCount());

  feed_state::RetryDue out;
  TEST_ASSERT_FALSE(feed_state::journalDue(anchorEpoch() + 29, out));
  TEST_ASSERT_TRUE(feed_state::journalDue(anchorEpoch() + 30, out));
  TEST_ASSERT_EQUAL_UINT8(7, out.hour);
  TEST_ASSERT_EQUAL_UINT8(30, out.minute);
  TEST_ASSERT_EQUAL_UINT16(3, out.grams); // v1.10.0: portion (g) carried
  TEST_ASSERT_EQUAL_UINT8(0x01, out.tankMask);
}

void test_journal_inflight_not_handed_out_twice_and_clear_works(void) {
  feed_state::journalArm(7, 30, 3, 0x01);
  feed_state::RetryDue out;
  TEST_ASSERT_TRUE(feed_state::journalDue(anchorEpoch() + 30, out));
  // in flight (dueEpoch 0): quiet until resolved
  TEST_ASSERT_FALSE(feed_state::journalDue(anchorEpoch() + 60, out));
  TEST_ASSERT_EQUAL_INT(1, feed_state::journalCount());

  feed_state::journalClearFor(7, 30); // the retry succeeded
  TEST_ASSERT_EQUAL_INT(0, feed_state::journalCount());
  TEST_ASSERT_FALSE(feed_state::journalDue(anchorEpoch() + 120, out));
}

void test_journal_fail_backoff_ladder_then_give_up(void) {
  const uint32_t t0 = anchorEpoch();
  feed_state::journalArm(7, 30, 3, 0x01); // attempt 1 (scheduled try failed)

  feed_state::RetryDue out;
  g_now = makeLocalTime(2025, 6, 16, 12, 1, 0); // t0 + 60
  TEST_ASSERT_TRUE(feed_state::journalDue(t0 + 60, out));
  TEST_ASSERT_TRUE(feed_state::journalFail(7, 30)); // attempt 2 -> +300 s
  TEST_ASSERT_FALSE(feed_state::journalDue(t0 + 60 + 299, out));
  TEST_ASSERT_TRUE(feed_state::journalDue(t0 + 60 + 300, out));

  TEST_ASSERT_TRUE(feed_state::journalFail(7, 30)); // attempt 3 -> +900 s
  TEST_ASSERT_FALSE(feed_state::journalDue(t0 + 60 + 899, out));
  TEST_ASSERT_TRUE(feed_state::journalDue(t0 + 60 + 900, out));

  // 4th attempt total -> journal gives up and deactivates the slot
  TEST_ASSERT_FALSE(feed_state::journalFail(7, 30));
  TEST_ASSERT_EQUAL_INT(0, feed_state::journalCount());
  TEST_ASSERT_FALSE(feed_state::journalDue(t0 + 60 + 5000, out));
}

void test_journal_two_slots_and_same_identity_refresh(void) {
  TEST_ASSERT_TRUE(feed_state::journalArm(7, 30, 3, 0x01));
  TEST_ASSERT_TRUE(feed_state::journalArm(8, 0, 5, 0x02));
  TEST_ASSERT_EQUAL_INT(2, feed_state::journalCount());

  // both slots full and no (h,m) match -> rejected
  TEST_ASSERT_FALSE(feed_state::journalArm(9, 0, 3, 0x01));
  TEST_ASSERT_EQUAL_INT(2, feed_state::journalCount());

  // re-arming the same identity refreshes instead of duplicating
  TEST_ASSERT_TRUE(feed_state::journalArm(7, 30, 3, 0x01));
  TEST_ASSERT_EQUAL_INT(2, feed_state::journalCount());

  // refreshed slot restarts at the first (30 s) backoff
  feed_state::RetryDue out;
  TEST_ASSERT_FALSE(feed_state::journalDue(anchorEpoch() + 29, out));
  TEST_ASSERT_TRUE(feed_state::journalDue(anchorEpoch() + 30, out));
}

void test_journal_survives_reinit_from_prefs(void) {
  const uint32_t t0 = anchorEpoch();
  TEST_ASSERT_TRUE(feed_state::journalArm(7, 30, 3, 0x01));
  TEST_ASSERT_TRUE(feed_state::journalArm(8, 0, 5, 0x04));

  // "power cycle": re-init reloads the blobs the mock prefs now hold
  feed_state::init();
  TEST_ASSERT_EQUAL_INT(2, feed_state::journalCount());

  feed_state::RetryDue out;
  TEST_ASSERT_FALSE(feed_state::journalDue(t0 + 29, out)); // backoff preserved
  TEST_ASSERT_TRUE(feed_state::journalDue(t0 + 30, out));
  TEST_ASSERT_EQUAL_UINT8(7, out.hour);
  TEST_ASSERT_EQUAL_UINT8(30, out.minute);
  TEST_ASSERT_EQUAL_UINT16(3, out.grams); // v1.10.0: portion (g) carried
  TEST_ASSERT_EQUAL_UINT8(0x01, out.tankMask);
}

void test_journal_inflight_slot_becomes_due_after_reinit(void) {
  const uint32_t t0 = anchorEpoch();
  TEST_ASSERT_TRUE(feed_state::journalArm(7, 30, 3, 0x01));
  feed_state::RetryDue out;
  TEST_ASSERT_TRUE(feed_state::journalDue(t0 + 30, out)); // handed out -> dueEpoch 0

  // power died before the retry resolved; reboot 15 s later
  g_now = makeLocalTime(2025, 6, 16, 12, 0, 45);
  feed_state::init();
  TEST_ASSERT_FALSE(feed_state::journalDue(t0 + 44, out));
  TEST_ASSERT_TRUE(feed_state::journalDue(t0 + 45, out)); // fires right away
  TEST_ASSERT_EQUAL_UINT8(7, out.hour);
}

// v1.10.0 regression: a pre-1.10 journal blob stores pulse SECONDS ("s" key);
// init() must migrate it to grams via catfishSecondsToGrams + the tank's g/s.
void test_journal_legacy_s_blob_migrates_to_grams(void) {
  const uint32_t t0 = anchorEpoch();
  char blob[160];
  snprintf(blob, sizeof(blob),
           "[{\"h\":7,\"m\":30,\"s\":3,\"t\":1,\"a\":1,\"due\":%lu}]",
           (unsigned long)(t0 + 30));
  prefs::g_retry = blob;
  feed_state::init(); // "power cycle" reload from the legacy blob

  feed_state::RetryDue out;
  TEST_ASSERT_FALSE(feed_state::journalDue(t0 + 29, out));
  TEST_ASSERT_TRUE(feed_state::journalDue(t0 + 30, out));
  TEST_ASSERT_EQUAL_UINT16(15, out.grams); // 3 s at the default 5 g/s
  TEST_ASSERT_EQUAL_UINT8(0x01, out.tankMask);

  // an explicit tank rate is honoured by the migration: 3 s * 10 g/s = 30 g
  prefs::g_retry = blob;
  prefs::g_gps[1] = 10;
  feed_state::init();
  TEST_ASSERT_TRUE(feed_state::journalDue(t0 + 30, out));
  TEST_ASSERT_EQUAL_UINT16(30, out.grams);
}

// ---------------------------------------------------------------------------
// E4: skip-next
// ---------------------------------------------------------------------------
void test_skip_next_consumed_exactly_once(void) {
  feed_state::setSkipNext(7, 30);
  TEST_ASSERT_TRUE(feed_state::consumeSkip(7, 30));
  TEST_ASSERT_FALSE(feed_state::consumeSkip(7, 30)); // marker is one-shot
}

void test_skip_next_other_identity_does_not_consume(void) {
  feed_state::setSkipNext(7, 30);
  TEST_ASSERT_FALSE(feed_state::consumeSkip(8, 0)); // different HH:MM
  TEST_ASSERT_TRUE(feed_state::consumeSkip(7, 30)); // still armed afterwards
}

// ---------------------------------------------------------------------------
// A4: catch-up scan
// ---------------------------------------------------------------------------
void test_scan_catchup_fires_within_window_once(void) {
  FeedTime entries[1] = {FeedTime{7, 30, true, 3, 0x01}};
  g_now = makeLocalTime(2025, 6, 16, 7, 35, 0); // 5 min after a daily 07:30

  feed_state::CatchupDue out[2];
  TEST_ASSERT_EQUAL_INT(1, feed_state::scanCatchup(entries, 1, (uint32_t)g_now,
                                                   10, out, 2));
  TEST_ASSERT_EQUAL_UINT8(7, out[0].hour);
  TEST_ASSERT_EQUAL_UINT8(30, out[0].minute);
  TEST_ASSERT_EQUAL_UINT16(3, out[0].grams); // portion carried (was .seconds)
  TEST_ASSERT_EQUAL_UINT8(0x01, out[0].tankMask);

  // marked fired BEFORE returning -> second scan (and a reboot) stay quiet
  TEST_ASSERT_EQUAL_INT(0, feed_state::scanCatchup(entries, 1, (uint32_t)g_now,
                                                   10, out, 2));
  feed_state::init(); // fired map round-trips through the mock prefs
  TEST_ASSERT_EQUAL_INT(0, feed_state::scanCatchup(entries, 1, (uint32_t)g_now,
                                                   10, out, 2));
}

void test_scan_catchup_outside_window_and_disabled_entry(void) {
  FeedTime entries[1] = {FeedTime{7, 30, true, 3, 0x01}};
  g_now = makeLocalTime(2025, 6, 16, 7, 46, 0); // 16 min late, window 10
  feed_state::CatchupDue out[2];
  TEST_ASSERT_EQUAL_INT(0, feed_state::scanCatchup(entries, 1, (uint32_t)g_now,
                                                   10, out, 2));

  // disabled entries never catch up
  FeedTime off[1] = {FeedTime{7, 30, false, 3, 0x01}};
  g_now = makeLocalTime(2025, 6, 16, 7, 35, 0);
  TEST_ASSERT_EQUAL_INT(0, feed_state::scanCatchup(off, 1, (uint32_t)g_now, 10,
                                                   out, 2));
}

void test_scan_catchup_window_zero_disables_scan(void) {
  FeedTime entries[1] = {FeedTime{7, 30, true, 3, 0x01}};
  g_now = makeLocalTime(2025, 6, 16, 7, 35, 0);
  feed_state::CatchupDue out[2];
  TEST_ASSERT_EQUAL_INT(0, feed_state::scanCatchup(entries, 1, (uint32_t)g_now,
                                                   0, out, 2));
  TEST_ASSERT_EQUAL_INT(0, (int)prefs::g_fired.size()); // nothing marked fired
}

void test_scan_catchup_respects_weekday_mask(void) {
  // Mon-only 07:30 seen on Tuesday 07:35: the latest covered occurrence was
  // YESTERDAY (~24.5 h late) — far outside the 10-minute window.
  FeedTime entries[1] = {FeedTime{7, 30, true, 3, 0x01}};
  entries[0].days = 0x01;
  g_now = makeLocalTime(2025, 6, 17, 7, 35, 0); // Tuesday
  feed_state::CatchupDue out[2];
  TEST_ASSERT_EQUAL_INT(0, feed_state::scanCatchup(entries, 1, (uint32_t)g_now,
                                                   10, out, 2));
  TEST_ASSERT_EQUAL_INT(0, (int)prefs::g_fired.size()); // not marked either
}

void test_scan_catchup_skips_already_fired_identity(void) {
  FeedTime entries[1] = {FeedTime{7, 30, true, 3, 0x01}};
  g_now = makeLocalTime(2025, 6, 16, 7, 35, 0);
  // the 1 Hz tick fired it on time; the scanner must not double-fire
  feed_state::markFired(7, 30, (uint32_t)g_now);
  feed_state::CatchupDue out[2];
  TEST_ASSERT_EQUAL_INT(0, feed_state::scanCatchup(entries, 1, (uint32_t)g_now,
                                                   10, out, 2));
}

// ---------------------------------------------------------------------------
// C3: counters (daily reset at local midnight, lifetime kept)
// ---------------------------------------------------------------------------
void test_counters_daily_and_lifetime(void) {
  feed_state::onFeedResult(true, 5);
  feed_state::onFeedResult(true, 5);
  feed_state::onFeedResult(false, 3);
  TEST_ASSERT_EQUAL_UINT16(2, feed_state::feedsOkToday());
  TEST_ASSERT_EQUAL_UINT16(1, feed_state::feedsFailToday());
  TEST_ASSERT_EQUAL_UINT32(2, feed_state::totalFeeds());
  TEST_ASSERT_EQUAL_UINT32(10, feed_state::totalSeconds());
  TEST_ASSERT_EQUAL_UINT32(20250616, feed_state::statsDay()); // YYYYMMDD local
}

void test_counters_roll_past_midnight_and_persist_same_day(void) {
  feed_state::onFeedResult(true, 5);
  feed_state::onFeedResult(false, 3);

  // same-day reboot: counters survive (blob day == today -> no roll)
  feed_state::init();
  TEST_ASSERT_EQUAL_UINT16(1, feed_state::feedsOkToday());
  TEST_ASSERT_EQUAL_UINT16(1, feed_state::feedsFailToday());
  TEST_ASSERT_EQUAL_UINT32(1, feed_state::totalFeeds());

  // cross local midnight: today counters reset, lifetime counters kept
  g_now = makeLocalTime(2025, 6, 17, 0, 5, 0); // Tuesday 00:05
  feed_state::onFeedResult(true, 1);           // triggers the roll first
  TEST_ASSERT_EQUAL_UINT16(1, feed_state::feedsOkToday()); // 0 rolled + 1
  TEST_ASSERT_EQUAL_UINT16(0, feed_state::feedsFailToday());
  TEST_ASSERT_EQUAL_UINT32(2, feed_state::totalFeeds());
  TEST_ASSERT_EQUAL_UINT32(6, feed_state::totalSeconds());
  TEST_ASSERT_EQUAL_UINT32(20250617, feed_state::statsDay());
}

// ---------------------------------------------------------------------------
// F3/A5: policy guards
// ---------------------------------------------------------------------------
void test_guards_vacation_blocks_all_but_touch(void) {
  prefs::setVacation(true);
  feed_state::GuardResult g =
      feed_state::checkFeed(feed_state::SRC_REMOTE, (uint32_t)g_now);
  TEST_ASSERT_FALSE(g.allowed);
  TEST_ASSERT_EQUAL_STRING("vacation", g.reason);
  g = feed_state::checkFeed(feed_state::SRC_SCHED, (uint32_t)g_now);
  TEST_ASSERT_FALSE(g.allowed);
  TEST_ASSERT_EQUAL_STRING("vacation", g.reason);
  g = feed_state::checkFeed(feed_state::SRC_CATCHUP, (uint32_t)g_now);
  TEST_ASSERT_FALSE(g.allowed);
  TEST_ASSERT_EQUAL_STRING("vacation", g.reason);
  // on-device touch: a human is standing right there -> always allowed
  g = feed_state::checkFeed(feed_state::SRC_TOUCH, (uint32_t)g_now);
  TEST_ASSERT_TRUE(g.allowed);
}

void test_guards_min_gap(void) {
  prefs::setMinGapMin(10);
  const uint32_t lastFed = (uint32_t)makeLocalTime(2025, 6, 16, 11, 55, 0);
  prefs::g_lastFed = lastFed; // 5 min before the anchor (Monday 12:00)

  feed_state::GuardResult g =
      feed_state::checkFeed(feed_state::SRC_REMOTE, (uint32_t)g_now);
  TEST_ASSERT_FALSE(g.allowed);
  TEST_ASSERT_EQUAL_STRING("min gap", g.reason);

  // at exactly the gap boundary the feed opens up again
  g = feed_state::checkFeed(feed_state::SRC_REMOTE, lastFed + 10 * 60);
  TEST_ASSERT_TRUE(g.allowed);
}

void test_guards_daily_max(void) {
  prefs::setMaxFeedsDay(2);
  feed_state::onFeedResult(true, 3);
  feed_state::GuardResult g =
      feed_state::checkFeed(feed_state::SRC_SCHED, (uint32_t)g_now);
  TEST_ASSERT_TRUE(g.allowed); // 1 < 2

  feed_state::onFeedResult(true, 3);
  g = feed_state::checkFeed(feed_state::SRC_CATCHUP, (uint32_t)g_now);
  TEST_ASSERT_FALSE(g.allowed);
  TEST_ASSERT_EQUAL_STRING("daily max", g.reason);
  TEST_ASSERT_TRUE(
      feed_state::checkFeed(feed_state::SRC_TOUCH, (uint32_t)g_now).allowed);
}

void test_guards_defaults_allow_everything(void) {
  // setUp: no vacation, gap 0, max 0
  feed_state::GuardResult g =
      feed_state::checkFeed(feed_state::SRC_REMOTE, (uint32_t)g_now);
  TEST_ASSERT_TRUE(g.allowed);
  TEST_ASSERT_EQUAL_STRING("", g.reason);
  TEST_ASSERT_TRUE(
      feed_state::checkFeed(feed_state::SRC_SCHED, (uint32_t)g_now).allowed);
  TEST_ASSERT_TRUE(
      feed_state::checkFeed(feed_state::SRC_CATCHUP, (uint32_t)g_now).allowed);
}

// ---------------------------------------------------------------------------
// D1: hopper estimator
// ---------------------------------------------------------------------------
void test_hopper_usage_accumulates_and_refills(void) {
  const uint16_t grams[4] = {5, 7, 0, 0};
  feed_state::hopperAddUsage(0x03, grams); // tanks 1+2
  TEST_ASSERT_EQUAL_UINT16(5, feed_state::hopperUsedG(1));
  TEST_ASSERT_EQUAL_UINT16(7, feed_state::hopperUsedG(2));
  feed_state::hopperAddUsage(0x01, grams); // tank 1 again
  TEST_ASSERT_EQUAL_UINT16(10, feed_state::hopperUsedG(1));

  // usage survives a reboot (hopper blob round-trip)
  feed_state::init();
  TEST_ASSERT_EQUAL_UINT16(10, feed_state::hopperUsedG(1));
  TEST_ASSERT_EQUAL_UINT16(7, feed_state::hopperUsedG(2));

  feed_state::hopperRefill(1); // refill tank 1 only
  TEST_ASSERT_EQUAL_UINT16(0, feed_state::hopperUsedG(1));
  TEST_ASSERT_EQUAL_UINT16(7, feed_state::hopperUsedG(2));

  feed_state::hopperRefill(0xFF); // refill everything
  TEST_ASSERT_EQUAL_UINT16(0, feed_state::hopperUsedG(2));
}

void test_hopper_level_pct_and_low_alert(void) {
  // no capacity configured -> 255 (unknown), never triggers "low"
  TEST_ASSERT_EQUAL_UINT8(255, feed_state::hopperLevelPct(1));
  TEST_ASSERT_FALSE(feed_state::hopperLowAny(1));

  prefs::g_cap[1] = 600; // tank 1 holds 600 g (v1.9.0 stored pulse-seconds)
  TEST_ASSERT_EQUAL_UINT8(100, feed_state::hopperLevelPct(1)); // unused

  // 255 g + 45 g = 300 g used of 600 g -> exactly 50 %
  const uint16_t first[4] = {255, 0, 0, 0};
  const uint16_t rest[4] = {45, 0, 0, 0};
  feed_state::hopperAddUsage(0x01, first);
  feed_state::hopperAddUsage(0x01, rest);
  TEST_ASSERT_EQUAL_UINT8(50, feed_state::hopperLevelPct(1));

  // default low threshold (20 %): 50 % is still fine
  TEST_ASSERT_FALSE(feed_state::hopperLowAny(1));

  const uint16_t more[4] = {250, 0, 0, 0}; // 300 + 250 = 550/600 -> 9 %
  feed_state::hopperAddUsage(0x01, more);
  TEST_ASSERT_EQUAL_UINT8(9, feed_state::hopperLevelPct(1));
  TEST_ASSERT_TRUE(feed_state::hopperLowAny(1));

  // over-capacity clamps to 0 %
  feed_state::hopperRefill(1);
  const uint16_t over[4] = {255, 0, 0, 0};
  feed_state::hopperAddUsage(0x01, over);
  feed_state::hopperAddUsage(0x01, over);
  feed_state::hopperAddUsage(0x01, over); // 765 g >= 600 g
  TEST_ASSERT_EQUAL_UINT8(0, feed_state::hopperLevelPct(1));
}

void test_hopper_low_ignores_unconfigured_tanks(void) {
  prefs::g_cap[2] = 600; // only tank 2 has a known capacity
  // 255 g + 255 g + 40 g = 550 g used of 600 g -> 9 % (below 20 %)
  const uint16_t g255[4] = {0, 255, 0, 0};
  const uint16_t g40[4] = {0, 40, 0, 0};
  feed_state::hopperAddUsage(0x02, g255);
  feed_state::hopperAddUsage(0x02, g255);
  feed_state::hopperAddUsage(0x02, g40);
  TEST_ASSERT_TRUE(feed_state::hopperLowAny(2));  // tank 2 is in the scan
  TEST_ASSERT_FALSE(feed_state::hopperLowAny(1)); // scan stops at tank 1
}

// ---------------------------------------------------------------------------
// C1: history ring
// ---------------------------------------------------------------------------
void test_history_ring_caps_at_24_newest_first(void) {
  for (int i = 0; i < 30; i++) {
    g_now = makeLocalTime(2025, 6, 16, 12, 0, i); // distinct epochs
    feed_state::recordHistory((uint8_t)(0x01 << (i % 4)), (uint8_t)(i + 1),
                              (uint16_t)(i + 1), (i % 2) == 1,
                              feed_state::HIST_MANUAL);
  }
  TEST_ASSERT_EQUAL_INT(24, feed_state::historyCount());

  const feed_state::HistEntry *h = feed_state::historyAll();
  TEST_ASSERT_EQUAL_UINT32((uint32_t)makeLocalTime(2025, 6, 16, 12, 0, 29),
                           h[0].epoch); // index 0 = newest
  TEST_ASSERT_EQUAL_UINT8(30, h[0].seconds);
  TEST_ASSERT_EQUAL_UINT16(30, h[0].grams);
  TEST_ASSERT_TRUE(h[0].ok);
  TEST_ASSERT_EQUAL_UINT8(feed_state::HIST_MANUAL, h[0].flag);
  // 6 oldest entries (i = 0..5) fell off; index 23 = event i = 6
  TEST_ASSERT_EQUAL_UINT8(7, h[23].seconds);
  TEST_ASSERT_EQUAL_UINT16(7, h[23].grams);
  TEST_ASSERT_EQUAL_UINT8(0x04, h[23].tankMask);
  TEST_ASSERT_FALSE(h[23].ok);
}

void test_history_dirty_flag_lifecycle(void) {
  TEST_ASSERT_FALSE(feed_state::historyDirty());
  feed_state::recordHistory(0x01, 3, 0, true, feed_state::HIST_MANUAL);
  TEST_ASSERT_TRUE(feed_state::historyDirty());
  feed_state::historyClearDirty();
  TEST_ASSERT_FALSE(feed_state::historyDirty());

  // serializing does not clear the flag (the consumer does)
  feed_state::recordHistory(0x01, 3, 0, true, feed_state::HIST_MANUAL);
  (void)feed_state::historyJson(1);
  TEST_ASSERT_TRUE(feed_state::historyDirty());
}

void test_history_json_roundtrip_via_arduinojson(void) {
  g_now = makeLocalTime(2025, 6, 16, 12, 0, 0);
  feed_state::recordHistory(0x03, 4, 40, true, feed_state::HIST_RETRY);
  g_now = makeLocalTime(2025, 6, 16, 12, 0, 10);
  feed_state::recordHistory(0x02, 9, 0, false, feed_state::HIST_BLOCKED);

  const String j = feed_state::historyJson(2); // newest-first
  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, j) == DeserializationError::Ok);
  TEST_ASSERT_TRUE(doc.is<JsonArray>());
  JsonArray arr = doc.as<JsonArray>();
  TEST_ASSERT_EQUAL_INT(2, (int)arr.size());

  // [0] = newest: epoch+10, tank 2, 9 s, failed, blocked
  TEST_ASSERT_EQUAL_UINT32(
      (uint32_t)makeLocalTime(2025, 6, 16, 12, 0, 10),
      (uint32_t)(long)(arr[0]["e"] | 0L));
  TEST_ASSERT_EQUAL_UINT8(0x02, (uint8_t)(arr[0]["m"] | 0));
  TEST_ASSERT_EQUAL_UINT8(9, (uint8_t)(arr[0]["s"] | 0));
  TEST_ASSERT_EQUAL_INT(0, (int)(arr[0]["g"] | -1)); // blocked: no grams known
  TEST_ASSERT_EQUAL_INT(0, (int)(arr[0]["ok"] | -1));
  TEST_ASSERT_EQUAL_UINT8(feed_state::HIST_BLOCKED,
                          (uint8_t)(arr[0]["f"] | 0));
  // [1] = older: tank 1+2, 4 s, ok, retry
  TEST_ASSERT_EQUAL_UINT8(0x03, (uint8_t)(arr[1]["m"] | 0));
  TEST_ASSERT_EQUAL_UINT8(4, (uint8_t)(arr[1]["s"] | 0));
  TEST_ASSERT_EQUAL_INT(40, (int)(arr[1]["g"] | -1)); // v1.10.0: grams present
  TEST_ASSERT_EQUAL_INT(1, (int)(arr[1]["ok"] | -1));
  TEST_ASSERT_EQUAL_UINT8(feed_state::HIST_RETRY, (uint8_t)(arr[1]["f"] | 0));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_journal_due_gates_on_first_backoff);
  RUN_TEST(test_journal_inflight_not_handed_out_twice_and_clear_works);
  RUN_TEST(test_journal_fail_backoff_ladder_then_give_up);
  RUN_TEST(test_journal_two_slots_and_same_identity_refresh);
  RUN_TEST(test_journal_survives_reinit_from_prefs);
  RUN_TEST(test_journal_inflight_slot_becomes_due_after_reinit);
  RUN_TEST(test_journal_legacy_s_blob_migrates_to_grams);
  RUN_TEST(test_skip_next_consumed_exactly_once);
  RUN_TEST(test_skip_next_other_identity_does_not_consume);
  RUN_TEST(test_scan_catchup_fires_within_window_once);
  RUN_TEST(test_scan_catchup_outside_window_and_disabled_entry);
  RUN_TEST(test_scan_catchup_window_zero_disables_scan);
  RUN_TEST(test_scan_catchup_respects_weekday_mask);
  RUN_TEST(test_scan_catchup_skips_already_fired_identity);
  RUN_TEST(test_counters_daily_and_lifetime);
  RUN_TEST(test_counters_roll_past_midnight_and_persist_same_day);
  RUN_TEST(test_guards_vacation_blocks_all_but_touch);
  RUN_TEST(test_guards_min_gap);
  RUN_TEST(test_guards_daily_max);
  RUN_TEST(test_guards_defaults_allow_everything);
  RUN_TEST(test_hopper_usage_accumulates_and_refills);
  RUN_TEST(test_hopper_level_pct_and_low_alert);
  RUN_TEST(test_hopper_low_ignores_unconfigured_tanks);
  RUN_TEST(test_history_ring_caps_at_24_newest_first);
  RUN_TEST(test_history_dirty_flag_lifecycle);
  RUN_TEST(test_history_json_roundtrip_via_arduinojson);
  return UNITY_END();
}
