/**
 * @file Arduino.h
 * @brief Minimal host-side mock of the Arduino core header for native tests.
 *
 * Compiled ONLY in the PlatformIO `native` environment (see platformio.ini).
 * Provides exactly what feed_scheduler and its prefs dependency use:
 *   - String (aliased to std::string — ArduinoJson serializes to it natively)
 *   - a Serial mock (printf passthrough to stdout, println)
 *   - time_t plumbing via the host <time.h>
 */
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <time.h>

using String = std::string;

struct MockSerial {
  void begin(unsigned long) {}
  void println() { std::fputc('\n', stdout); }
  void println(const char *s) { std::printf("%s\n", s); }
  void println(const String &s) { std::printf("%s\n", s.c_str()); }
  template <typename... Args>
  void printf(const char *fmt, Args... args) {
    std::fprintf(stdout, fmt, args...);
  }
};

// Internal linkage per translation unit: the scheduler and the test each get
// an independent (stateless) instance, which is all they need.
static MockSerial Serial;
