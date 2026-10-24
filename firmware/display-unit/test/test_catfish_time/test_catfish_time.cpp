/**
 * @file test_catfish_time.cpp
 * @brief Host-side unit tests for the shared time helpers (v1.6.0).
 *
 * Runs with:  pio test -d firmware/display-unit -e native
 *
 * Covers:
 *  - catfishFormatHm(): 24h passthrough, 12h transform incl. midnight/noon
 *    edge cases, range rejection, buffer-safety.
 *  - civil <-> epoch conversion used by the PCF85063 RTC driver: known
 *    epochs, leap years, roundtrips across the 2000..2099 RTC range.
 */
#include <unity.h>

#include <catfish_time.h>
#include <stdio.h>

// ---------------------------------------------------------------- formatHm
void test_fmt24_passthrough(void) {
  char b[12];
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 0, 0, false));
  TEST_ASSERT_EQUAL_STRING("00:00", b);
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 7, 30, false));
  TEST_ASSERT_EQUAL_STRING("07:30", b);
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 23, 59, false));
  TEST_ASSERT_EQUAL_STRING("23:59", b);
}

void test_fmt12_edges(void) {
  char b[12];
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 0, 0, true));
  TEST_ASSERT_EQUAL_STRING("12:00 AM", b); // midnight
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 0, 5, true));
  TEST_ASSERT_EQUAL_STRING("12:05 AM", b);
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 12, 0, true));
  TEST_ASSERT_EQUAL_STRING("12:00 PM", b); // noon
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 12, 59, true));
  TEST_ASSERT_EQUAL_STRING("12:59 PM", b);
}

void test_fmt12_am_pm_split(void) {
  char b[12];
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 1, 5, true));
  TEST_ASSERT_EQUAL_STRING("1:05 AM", b); // no hour zero-pad in 12h
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 11, 59, true));
  TEST_ASSERT_EQUAL_STRING("11:59 AM", b);
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 13, 5, true));
  TEST_ASSERT_EQUAL_STRING("1:05 PM", b);
  TEST_ASSERT_TRUE(catfishFormatHm(b, sizeof(b), 23, 59, true));
  TEST_ASSERT_EQUAL_STRING("11:59 PM", b);
}

void test_fmt_rejects_out_of_range(void) {
  char b[12];
  b[0] = 'x';
  TEST_ASSERT_FALSE(catfishFormatHm(b, sizeof(b), 24, 0, false));
  TEST_ASSERT_EQUAL_STRING("", b);
  TEST_ASSERT_FALSE(catfishFormatHm(b, sizeof(b), -1, 0, true));
  TEST_ASSERT_FALSE(catfishFormatHm(b, sizeof(b), 12, 60, true));
  TEST_ASSERT_FALSE(catfishFormatHm(b, sizeof(b), 12, -1, true));
}

void test_fmt_buffer_safety(void) {
  char small[4]; // too small for "7:05 AM"
  TEST_ASSERT_FALSE(catfishFormatHm(small, sizeof(small), 7, 5, true));
  TEST_ASSERT_EQUAL_STRING("", small);
  char ok[12];
  TEST_ASSERT_TRUE(catfishFormatHm(ok, sizeof(ok), 7, 5, true));
  TEST_ASSERT_EQUAL_STRING("7:05 AM", ok);
  TEST_ASSERT_FALSE(catfishFormatHm(nullptr, 12, 7, 5, false));
}

// ---------------------------------------------------------------- civil/epoch
void test_epoch_known_values(void) {
  CatfishCivil c;
  c.year = 1970; c.month = 1; c.day = 1;
  c.hour = 0; c.minute = 0; c.second = 0;
  TEST_ASSERT_EQUAL_INT64(0, catfishTimeCivilToEpoch(c));

  c.year = 2000; c.month = 3; c.day = 1;
  c.hour = 0; c.minute = 0; c.second = 0;
  TEST_ASSERT_EQUAL_INT64(951868800LL, catfishTimeCivilToEpoch(c));

  c.year = 2024; c.month = 2; c.day = 29; // leap day
  c.hour = 12; c.minute = 34; c.second = 56;
  TEST_ASSERT_EQUAL_INT64(1709210096LL, catfishTimeCivilToEpoch(c));

  const CatfishCivil r = catfishTimeEpochToCivil(1709210096LL);
  TEST_ASSERT_EQUAL_INT32(2024, r.year);
  TEST_ASSERT_EQUAL_UINT8(2, r.month);
  TEST_ASSERT_EQUAL_UINT8(29, r.day);
  TEST_ASSERT_EQUAL_UINT8(12, r.hour);
  TEST_ASSERT_EQUAL_UINT8(34, r.minute);
  TEST_ASSERT_EQUAL_UINT8(56, r.second);
}

void test_civil_epoch_roundtrip(void) {
  // sweep the RTC-relevant 2000..2099 range at 111-day steps
  for (int64_t e = 946684800LL; e < 4102444800LL; e += 111LL * 86400LL) {
    const CatfishCivil c = catfishTimeEpochToCivil(e);
    char msg[64];
    snprintf(msg, sizeof(msg), "roundtrip @ %lld", (long long)e);
    TEST_ASSERT_TRUE_MESSAGE(c.month >= 1 && c.month <= 12, msg);
    TEST_ASSERT_TRUE_MESSAGE(c.day >= 1 && c.day <= 31, msg);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(e, catfishTimeCivilToEpoch(c), msg);
  }
}

void test_day_boundary_roundtrip(void) {
  // every hour on several month boundaries (leap + non-leap)
  const int64_t samples[] = {
      946684800LL,  // 2000-01-01
      951868800LL,  // 2000-03-01 (post-leap-day)
      1709164800LL, // 2024-02-29
      1735689600LL, // 2025-01-01
      4102444799LL, // 2099-12-31 23:59:59
  };
  for (int64_t base : samples) {
    for (int64_t e = base; e < base + 86400LL; e += 3600LL) {
      const CatfishCivil c = catfishTimeEpochToCivil(e);
      TEST_ASSERT_EQUAL_INT64_MESSAGE(e, catfishTimeCivilToEpoch(c),
                                      "hourly roundtrip");
    }
  }
}

void test_negative_epoch_midnight_floor(void) {
  // 1969-12-31 23:59:59 must survive the floor-division path
  const CatfishCivil c = catfishTimeEpochToCivil(-1LL);
  TEST_ASSERT_EQUAL_INT32(1969, c.year);
  TEST_ASSERT_EQUAL_UINT8(12, c.month);
  TEST_ASSERT_EQUAL_UINT8(31, c.day);
  TEST_ASSERT_EQUAL_UINT8(23, c.hour);
  TEST_ASSERT_EQUAL_UINT8(59, c.minute);
  TEST_ASSERT_EQUAL_UINT8(59, c.second);
}

// ---------------------------------------------------------------- runner
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fmt24_passthrough);
  RUN_TEST(test_fmt12_edges);
  RUN_TEST(test_fmt12_am_pm_split);
  RUN_TEST(test_fmt_rejects_out_of_range);
  RUN_TEST(test_fmt_buffer_safety);
  RUN_TEST(test_epoch_known_values);
  RUN_TEST(test_civil_epoch_roundtrip);
  RUN_TEST(test_day_boundary_roundtrip);
  RUN_TEST(test_negative_epoch_midnight_floor);
  return UNITY_END();
}
