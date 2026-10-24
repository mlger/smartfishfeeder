/**
 * @file feed_scheduler.cpp
 * @brief Feeding schedule engine (JSON blob in NVS, 1 Hz firing logic).
 */
#include "feed_scheduler.h"
#include <ArduinoJson.h>
#include <time.h>
#include "prefs.h"

namespace {

FeedTime s_entries[feed_scheduler::MAX_FEED_TIMES];
// minute-of-day when each entry last fired (0xFFFF = never this minute)
uint16_t s_last_fired[feed_scheduler::MAX_FEED_TIMES];
int s_count = 0;

void resetFireTracking() {
  for (int i = 0; i < feed_scheduler::MAX_FEED_TIMES; i++) s_last_fired[i] = 0xFFFF;
}

void clampEntry(FeedTime &t) {
  if (t.hour > 23) t.hour = 23;
  if (t.minute > 59) t.minute = 59;
  if (t.seconds < 1) t.seconds = 1;
  if (t.seconds > 30) t.seconds = 30;
}

bool persist() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < s_count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["h"] = s_entries[i].hour;
    o["m"] = s_entries[i].minute;
    o["e"] = s_entries[i].enabled;
    o["s"] = s_entries[i].seconds;
  }
  String out;
  serializeJson(doc, out);
  prefs::setScheduleJson(out);
  return true;
}

void seedDefaults() {
  s_count = 2;
  s_entries[0] = FeedTime{7, 30, true, 3};   // 07:30, 3 s
  s_entries[1] = FeedTime{17, 30, true, 3};  // 17:30, 3 s
  resetFireTracking();
  persist();
  Serial.println("[FEED] first boot: seeded default schedule 07:30 + 17:30 (3 s)");
}

} // anonymous namespace

namespace feed_scheduler {

bool init() {
  String j = prefs::getScheduleJson();

  // Missing/blank blob or corrupt JSON -> first boot (or corruption): seed.
  if (j.length() == 0) {
    seedDefaults();
    return true;
  }

  JsonDocument doc;
  if (deserializeJson(doc, j) != DeserializationError::Ok) {
    Serial.println("[FEED] WARN: schedule JSON corrupt, reseeding defaults");
    seedDefaults();
    return true;
  }

  s_count = 0;
  for (JsonVariant v : doc.as<JsonArray>()) {
    if (s_count >= MAX_FEED_TIMES) break;
    FeedTime t;
    t.hour = (uint8_t)(v["h"] | 0);
    t.minute = (uint8_t)(v["m"] | 0);
    t.enabled = (bool)(v["e"] | true);
    t.seconds = (uint8_t)(v["s"] | 3);
    clampEntry(t);
    s_last_fired[s_count] = 0xFFFF;
    s_entries[s_count++] = t;
  }
  // NOTE: an explicit empty array "[]" (user deleted all entries) is respected
  // and NOT reseeded; only an empty/missing/corrupt blob triggers seeding.
  Serial.printf("[FEED] schedule loaded: %d entries\n", s_count);
  return true;
}

int getCount() { return s_count; }

const FeedTime *getAll() { return s_entries; }

bool getEntry(int index, FeedTime &out) {
  if (index < 0 || index >= s_count) return false;
  out = s_entries[index];
  return true;
}

bool add(const FeedTime &ft) {
  if (s_count >= MAX_FEED_TIMES) return false;
  FeedTime t = ft;
  clampEntry(t);
  s_last_fired[s_count] = 0xFFFF;
  s_entries[s_count++] = t;
  persist();
  return true;
}

bool remove(int index) {
  if (index < 0 || index >= s_count) return false;
  for (int i = index; i < s_count - 1; i++) {
    s_entries[i] = s_entries[i + 1];
    s_last_fired[i] = s_last_fired[i + 1];
  }
  s_count--;
  persist();
  return true;
}

bool setEnabled(int index, bool enabled) {
  if (index < 0 || index >= s_count) return false;
  s_entries[index].enabled = enabled;
  persist();
  return true;
}

bool toggle(int index) {
  if (index < 0 || index >= s_count) return false;
  s_entries[index].enabled = !s_entries[index].enabled;
  persist();
  return true;
}

bool timeValid() {
  time_t now = time(nullptr);
  struct tm lt;
  if (!localtime_r(&now, &lt)) return false;
  return lt.tm_year >= (2025 - 1900); // NTP-synced gate
}

FeedTime tick() {
  const FeedTime none = FeedTime{0, 0, false, 0};

  if (!timeValid() || s_count == 0) return none;

  time_t now = time(nullptr);
  struct tm lt;
  if (!localtime_r(&now, &lt)) return none;

  const int now_min_of_day = lt.tm_hour * 60 + lt.tm_min;
  if (now_min_of_day < 0 || now_min_of_day > 24 * 60) return none;

  for (int i = 0; i < s_count; i++) {
    const FeedTime &e = s_entries[i];
    if (!e.enabled) continue;
    const int entry_min = e.hour * 60 + e.minute;
    if (entry_min == now_min_of_day &&
        s_last_fired[i] != (uint16_t)now_min_of_day) {
      // fire at most once per calendar minute per entry
      s_last_fired[i] = (uint16_t)now_min_of_day;
      Serial.printf("[FEED] due entry %d: %02u:%02u (%u s)\n", i,
                    (unsigned)e.hour, (unsigned)e.minute, (unsigned)e.seconds);
      return e;
    }
  }
  return none;
}

NextFeed nextOccurrence() {
  NextFeed out{};
  out.valid = false;
  out.tomorrow = false;
  out.hour = 0;
  out.minute = 0;
  out.seconds = 0;

  if (!timeValid() || s_count == 0) return out;

  time_t now = time(nullptr);
  struct tm lt;
  if (!localtime_r(&now, &lt)) return out;

  const int now_min = lt.tm_hour * 60 + lt.tm_min;

  int best_today = -1, best_tomorrow = -1;
  uint8_t th = 0, tm_ = 0, wh = 0, wm = 0, ws = 0;

  for (int i = 0; i < s_count; i++) {
    const FeedTime &e = s_entries[i];
    if (!e.enabled) continue;
    const int em = e.hour * 60 + e.minute;
    if (em > now_min && (best_today < 0 || em < best_today)) {
      best_today = em;
      th = e.hour;
      tm_ = e.minute;
      ws = e.seconds;
    }
    if (best_tomorrow < 0 || em < best_tomorrow) {
      best_tomorrow = em;
      wh = e.hour;
      wm = e.minute;
    }
  }

  if (best_today >= 0) {
    out.valid = true;
    out.tomorrow = false;
    out.hour = th;
    out.minute = tm_;
    out.seconds = ws;
  } else if (best_tomorrow >= 0) {
    out.valid = true;
    out.tomorrow = true;
    out.hour = wh;
    out.minute = wm;
    out.seconds = 0;
  }
  return out;
}

} // namespace feed_scheduler
