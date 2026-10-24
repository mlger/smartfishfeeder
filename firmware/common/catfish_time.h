/**
 * @file catfish_time.h
 * @brief Dependency-free time helpers shared by the Catfish Feeder firmware
 *        and the host (native) test suite.
 *
 * Two concerns live here:
 *  1. Wall-clock formatting with the user's 12/24-hour preference (v1.6.0).
 *     The 24-hour form is the machine/protocol form ("07:30"); 12-hour is a
 *     presentation-only transform ("7:30 AM") — stored schedule minutes and
 *     every MQTT/HTTP payload stay 24-hour.
 *  2. Civil <-> epoch conversion used by the PCF85063 RTC driver (the chip
 *     stores BCD calendar fields, not epochs). Host-tested so the BCD and
 *     leap-year math is pinned without hardware.
 *
 * Everything is `inline`/header-only (same pattern as catfish_mech.h).
 */
#ifndef CATFISH_TIME_H
#define CATFISH_TIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/** Calendar fields in UTC (RTC range: years 2000..2099). */
struct CatfishCivil {
  int year;          // full year, e.g. 2026
  uint8_t month;     // 1..12
  uint8_t day;       // 1..31
  uint8_t hour;      // 0..23
  uint8_t minute;    // 0..59
  uint8_t second;    // 0..59
};

/** Days since 1970-01-01 for a proleptic-Gregorian civil date (Hinnant). */
inline int64_t catfishTimeDaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);            // [0, 399]
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

/** UTC epoch seconds for civil fields (no timezone involvement). */
inline int64_t catfishTimeCivilToEpoch(const CatfishCivil &c) {
  return catfishTimeDaysFromCivil(c.year, c.month, c.day) * 86400LL +
         (int64_t)c.hour * 3600LL + (int64_t)c.minute * 60LL +
         (int64_t)c.second;
}

/** Inverse of catfishTimeCivilToEpoch. Valid for any epoch > ~-2^40. */
inline CatfishCivil catfishTimeEpochToCivil(int64_t epoch) {
  // floor division (C++11 truncates toward zero — adjust negative epochs)
  int64_t days = epoch / 86400LL;
  int64_t rem = epoch % 86400LL;
  if (rem < 0) {
    rem += 86400LL;
    days -= 1;
  }
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);                 // [0, 146096]
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const int64_t yr = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);   // [0, 365]
  const unsigned mp = (5u * doy + 2u) / 153u;                        // [0, 11]
  const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;               // [1, 31]
  const unsigned m = (mp < 10u) ? (mp + 3u) : (mp - 9u);             // [1, 12]
  CatfishCivil c;
  c.year = (int)(yr + ((m <= 2u) ? 1 : 0));
  c.month = (uint8_t)m;
  c.day = (uint8_t)d;
  c.hour = (uint8_t)(rem / 3600LL);
  c.minute = (uint8_t)((rem % 3600LL) / 60LL);
  c.second = (uint8_t)(rem % 60LL);
  return c;
}

/**
 * Format hour/minute with the user's clock preference.
 *  hour12=false -> "07:30" (zero-padded 24-hour)
 *  hour12=true  -> "7:30 AM" / "12:05 PM" (no hour padding; midnight = 12 AM)
 * Bounds-checked: returns false (and writes "" when n > 0) if buf is too
 * small — "11:59 PM" needs 9 bytes including the NUL.
 */
inline bool catfishFormatHm(char *buf, size_t n, int hour, int minute,
                            bool hour12) {
  if (!buf || n == 0) return false;
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    buf[0] = '\0';
    return false;
  }
  if (!hour12) {
    const int w = snprintf(buf, n, "%02d:%02d", hour, minute);
    if (w >= 0 && (size_t)w < n) return true;
    buf[0] = '\0';
    return false;
  }
  int h12 = hour % 12;
  if (h12 == 0) h12 = 12;
  const char *ap = (hour < 12) ? "AM" : "PM";
  const int w = snprintf(buf, n, "%d:%02d %s", h12, minute, ap);
  if (w >= 0 && (size_t)w < n) return true;
  buf[0] = '\0';
  return false;
}

#endif // CATFISH_TIME_H
