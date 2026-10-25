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
// v1.9.0 FIX: absolute epoch-minute of the last fire per entry (0 = never).
// Previously this stored the MINUTE-OF-DAY, which never re-fired the same
// HH:MM on later days of an uptime (a 24/7 device fed each entry at most once
// per boot; the v1.9.0 catch-up scan masked it by firing 0-10 min late).
// epoch/60 is monotonic and day-agnostic: blocks re-fires within the same
// calendar minute, allows them across days and midnight.
uint32_t s_last_fired[feed_scheduler::MAX_FEED_TIMES];
int s_count = 0;
bool s_dirty = false;

// Injectable clock (test hook; nullptr = system time). See set_time_source().
time_t (*s_time_fn)() = nullptr;

time_t nowTime() { return s_time_fn ? s_time_fn() : time(nullptr); }

void resetFireTracking() {
  for (int i = 0; i < feed_scheduler::MAX_FEED_TIMES; i++) s_last_fired[i] = 0;
}

void clampEntry(FeedTime &t) {
  if (t.hour > 23) t.hour = 23;
  if (t.minute > 59) t.minute = 59;
  t.grams = catfishClampGrams(t.grams); // v1.10.0: portion 1..500 g
  t.tanks &= 0x0F;      // P5: only tanks 1..4 exist
  if (t.tanks == 0) t.tanks = 0x01;
  if (t.days == 0) t.days = 0x7F; // v1.9.0: empty mask = every day
  for (int i = 0; i < 4; i++) {
    // v1.10.0: per-tank gram override (0 = inherit the shared grams)
    if (t.gramTank[i] > CATFISH_MAX_GRAMS) t.gramTank[i] = CATFISH_MAX_GRAMS;
  }
}

/** Serialize + write to NVS now (call only from loop()/init context). */
bool persistNow() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < s_count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["h"] = s_entries[i].hour;
    o["m"] = s_entries[i].minute;
    o["e"] = s_entries[i].enabled;
    o["g"] = s_entries[i].grams; // v1.10.0: portion in grams
    o["t"] = s_entries[i].tanks; // P5: target-tank bitmask
    o["d"] = s_entries[i].days;  // v1.9.0: weekday mask (0x7F = daily)
    JsonArray gt = o["gt"].to<JsonArray>(); // v1.10.0: per-tank grams (0 = shared)
    for (int k = 0; k < 4; k++) gt.add(s_entries[i].gramTank[k]);
  }
  String out;
  serializeJson(doc, out);
  // v1.8.2: only clear the dirty flag when NVS actually took the write —
  // otherwise a failed persist silently loses the schedule until the next
  // mutation re-dirties it.
  if (prefs::setScheduleJson(out)) {
    s_dirty = false;
    return true;
  }
  return false;
}

void seedDefaults() {
  s_count = 2;
  s_entries[0] = FeedTime{};   // zero the C-struct parts (tanks, days init below)
  s_entries[0].hour = 7;
  s_entries[0].minute = 30;
  s_entries[0].enabled = true;
  s_entries[0].tanks = 0x01;
  s_entries[0].grams = CATFISH_DEFAULT_GRAMS; // 15 g (== legacy 3 s @ 5 g/s)
  s_entries[1] = FeedTime{};
  s_entries[1].hour = 17;
  s_entries[1].minute = 30;
  s_entries[1].enabled = true;
  s_entries[1].tanks = 0x01;
  s_entries[1].grams = CATFISH_DEFAULT_GRAMS;
  resetFireTracking();
  s_dirty = true;
  Serial.printf("[FEED] first boot: seeded default schedule 07:30 + 17:30 (%u g)\n",
                (unsigned)CATFISH_DEFAULT_GRAMS);
}

/** Parse one schedule entry from JSON (legacy-safe through v1.9.0 blobs). */
void parseEntry(JsonVariant v, FeedTime &t) {
  t.hour = (uint8_t)(v["h"] | 0);
  t.minute = (uint8_t)(v["m"] | 0);
  t.enabled = (bool)(v["e"] | true);
  t.tanks = (uint8_t)(v["t"] | 1); // P5: missing key = legacy blob = tank 1
  t.days = (uint8_t)(v["d"] | 0x7F); // v1.9.0: absent = every day
  t.grams = (uint16_t)(v["g"] | 0);
  if (t.grams == 0) {
    // v1.10.0 migration: legacy blobs stored SECONDS ("s" key, 1..30).
    // Interpret them at the nominal auger rate (5 g/s) so prior portions
    // keep approximately the same physical size (3 s -> 15 g).
    const uint8_t legacySec = (uint8_t)(v["s"] | 3);
    t.grams = (uint16_t)legacySec * CATFISH_DEFAULT_GPS;
  }
  for (int k = 0; k < 4; k++) t.gramTank[k] = 0;
  if (v["gt"].is<JsonArray>()) {
    int k = 0;
    for (JsonVariant s : v["gt"].as<JsonArray>()) {
      if (k >= 4) break;
      t.gramTank[k++] = (uint16_t)(s | 0);
    }
  }
  clampEntry(t);
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
    parseEntry(v, t);
    s_last_fired[s_count] = 0xFFFF;
    s_entries[s_count++] = t;
  }
  // NOTE: an explicit empty array "[]" (user deleted all entries) is respected
  // and NOT reseeded; only an empty/missing/corrupt blob triggers seeding.
  Serial.printf("[FEED] schedule loaded: %d entries\n", s_count);
  return true;
}

bool flushIfDirty() {
  if (!s_dirty) return false;
  return persistNow();
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
  // reject duplicate HH:MM (UI surfaces a clearer message than "full")
  for (int i = 0; i < s_count; i++) {
    if (s_entries[i].hour == t.hour && s_entries[i].minute == t.minute) return false;
  }
  s_last_fired[s_count] = 0xFFFF;
  s_entries[s_count++] = t;
  s_dirty = true;
  return true;
}

bool remove(int index) {
  if (index < 0 || index >= s_count) return false;
  for (int i = index; i < s_count - 1; i++) {
    s_entries[i] = s_entries[i + 1];
    s_last_fired[i] = s_last_fired[i + 1];
  }
  s_count--;
  s_dirty = true;
  return true;
}

bool removeByIdentity(uint8_t hour, uint8_t minute) {
  for (int i = 0; i < s_count; i++) {
    if (s_entries[i].hour == hour && s_entries[i].minute == minute) {
      return remove(i);
    }
  }
  return false;
}

bool setEnabled(int index, bool enabled) {
  if (index < 0 || index >= s_count) return false;
  s_entries[index].enabled = enabled;
  s_dirty = true;
  return true;
}

bool setEnabledByIdentity(uint8_t hour, uint8_t minute, bool enabled) {
  for (int i = 0; i < s_count; i++) {
    if (s_entries[i].hour == hour && s_entries[i].minute == minute) {
      return setEnabled(i, enabled);
    }
  }
  return false;
}

bool toggle(int index) {
  if (index < 0 || index >= s_count) return false;
  s_entries[index].enabled = !s_entries[index].enabled;
  s_dirty = true;
  return true;
}

bool replaceAll(const String &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  if (!doc.is<JsonArray>()) return false;

  // Validate the whole payload into a scratch buffer first: the live schedule
  // is only swapped when everything checks out (atomic replace).
  FeedTime tmp[MAX_FEED_TIMES];
  int n = 0;
  for (JsonVariant v : doc.as<JsonArray>()) {
    if (n >= MAX_FEED_TIMES) return false; // oversize payload rejected whole
    FeedTime t;
    parseEntry(v, t);
    for (int i = 0; i < n; i++) {
      if (tmp[i].hour == t.hour && tmp[i].minute == t.minute) return false;
    }
    tmp[n++] = t;
  }

  for (int i = 0; i < n; i++) {
    s_entries[i] = tmp[i];
    s_last_fired[i] = 0xFFFF;
  }
  s_count = n;
  s_dirty = true;
  Serial.printf("[FEED] schedule replaced (%d entries)\n", n);
  return true;
}

bool timeValid() {
  time_t now = nowTime();
  struct tm lt;
  if (!localtime_r(&now, &lt)) return false;
  return lt.tm_year >= (2025 - 1900); // NTP-synced gate
}

FeedTime tick() {
  FeedTime none{};
  none.enabled = false;

  if (!timeValid() || s_count == 0) return none;

  time_t now = nowTime();
  struct tm lt;
  if (!localtime_r(&now, &lt)) return none;

  const int now_min_of_day = lt.tm_hour * 60 + lt.tm_min;
  if (now_min_of_day < 0 || now_min_of_day > 24 * 60) return none;

  for (int i = 0; i < s_count; i++) {
    const FeedTime &e = s_entries[i];
    if (!e.enabled) continue;
    if (!entryCoversWday(e, lt.tm_wday)) continue; // v1.9.0: weekday mask
    const int entry_min = e.hour * 60 + e.minute;
    // v1.9.0: fire at most once per calendar minute, forever (epoch/60 stamp)
    const uint32_t now_abs_min = (uint32_t)(now / 60);
    if (entry_min == now_min_of_day && s_last_fired[i] != now_abs_min) {
      s_last_fired[i] = now_abs_min;
      Serial.printf("[FEED] due entry %d: %02u:%02u (%u g, days 0x%02X)\n", i,
                    (unsigned)e.hour, (unsigned)e.minute, (unsigned)e.grams,
                    (unsigned)e.days);
      return e;
    }
  }
  return none;
}

/**
 * v1.9.0: weekday-aware next occurrence. With weekday masks the next feed can
 * be several days out (e.g. a Mon-only entry seen on a Tuesday), so this scans
 * up to 8 days ahead instead of only today/tomorrow.
 */
NextFeed nextOccurrence() {
  NextFeed out{};
  out.valid = false;
  out.tomorrow = false;
  out.hour = 0;
  out.minute = 0;
  out.grams = 0;
  out.daysAhead = 0;

  if (!timeValid() || s_count == 0) return out;

  time_t now = nowTime();
  struct tm lt;
  if (!localtime_r(&now, &lt)) return out;

  const int now_min = lt.tm_hour * 60 + lt.tm_min;

  for (int d = 0; d < 8; d++) {
    const int wday = (lt.tm_wday + d) % 7;
    int best = -1;
    uint8_t bh = 0, bm = 0;
    uint16_t bg = 0;
    for (int i = 0; i < s_count; i++) {
      const FeedTime &e = s_entries[i];
      if (!e.enabled) continue;
      if (!entryCoversWday(e, wday)) continue;
      const int em = e.hour * 60 + e.minute;
      if (d == 0 && em <= now_min) continue; // today: strictly in the future
      if (best < 0 || em < best) {
        best = em;
        bh = e.hour;
        bm = e.minute;
        bg = e.grams;
      }
    }
    if (best >= 0) {
      out.valid = true;
      out.tomorrow = (d > 0);
      out.daysAhead = (uint8_t)d;
      out.hour = bh;
      out.minute = bm;
      out.grams = bg;
      return out;
    }
  }
  return out;
}

void set_time_source(time_t (*fn)()) { s_time_fn = fn; }

} // namespace feed_scheduler
