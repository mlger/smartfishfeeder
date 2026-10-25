/**
 * @file feed_state.cpp
 * @brief Persistent feeding state + policy implementation (v1.9.0).
 *
 * All state lives in RAM caches mirrored to NVS via prefs JSON blobs on every
 * mutation (writes are per feed EVENT, not per second — no wear concern).
 * See feed_state.h for the contract.
 */
#include "feed_state.h"

#include <ArduinoJson.h>
#include <time.h>

#include "prefs.h"

namespace {

constexpr int RETRY_SLOTS = 2;
constexpr int HIST_MAX = 24;
// Backoff between retry attempts, seconds. attempts counts feed attempts made
// for the slot (1 = just the initial scheduled try failed).
constexpr uint32_t RETRY_BACKOFF_S[] = {30, 300, 900};
constexpr int RETRY_MAX_ATTEMPTS = 1 + (int)(sizeof(RETRY_BACKOFF_S) /
                                             sizeof(RETRY_BACKOFF_S[0]));

struct RetrySlot {
  bool active = false;
  uint8_t hour = 0, minute = 0, tankMask = 0;
  uint16_t grams = 0; // v1.10.0: portion (g)
  uint8_t attempts = 0;
  uint32_t dueEpoch = 0; // 0 = in flight (a retry was handed out, not resolved)
};
RetrySlot s_retry[RETRY_SLOTS];

// Fired map: per-entry identity (HH:MM) -> last fired epoch.
struct FiredSlot {
  uint8_t hour = 0, minute = 0;
  uint32_t epoch = 0;
};
constexpr int FIRED_MAX = 12;
FiredSlot s_fired[FIRED_MAX];
int s_fired_n = 0;

// Daily counters (C3)
uint32_t s_day = 0; // YYYYMMDD local
uint16_t s_ok_today = 0, s_fail_today = 0;
uint32_t s_tot = 0, s_sec = 0;

// Hopper usage per tank, GRAMS since refill (D1; v1.9.0 was pulse-seconds —
// the NVS key changed from "hopper" to "hopG" so old sec-values never
// masquerade as grams; usage simply restarts at 0 after the upgrade).
uint16_t s_used[4] = {0, 0, 0, 0};

// History ring (C1): index 0 = newest (feed_state::HistEntry — anonymous
// namespace sits at file scope, so the nested struct needs qualification)
feed_state::HistEntry s_hist[HIST_MAX];
int s_hist_n = 0;
bool s_hist_dirty = false;

// Injectable clock (test hook; nullptr = system time) — same pattern as
// feed_scheduler::set_time_source(). Production code never calls it.
namespace {
time_t (*s_time_fn)() = nullptr;
time_t nowTime() { return s_time_fn ? s_time_fn() : time(nullptr); }
} // namespace

uint32_t dayStampOf(uint32_t epoch) {
  const time_t t = (time_t)epoch;
  struct tm lt;
  if (!localtime_r(&t, &lt)) return 0;
  return (uint32_t)((lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 +
                    lt.tm_mday);
}

uint32_t nowDayStamp() {
  return dayStampOf((uint32_t)nowTime());
}

// ------------------------------------------------------------ persistence
void persistRetry() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < RETRY_SLOTS; i++) {
    if (!s_retry[i].active) continue;
    JsonObject o = arr.add<JsonObject>();
    o["h"] = s_retry[i].hour;
    o["m"] = s_retry[i].minute;
    o["g"] = s_retry[i].grams;
    o["t"] = s_retry[i].tankMask;
    o["a"] = s_retry[i].attempts;
    o["due"] = s_retry[i].dueEpoch;
  }
  String out;
  serializeJson(doc, out);
  prefs::setRetryJson(out);
}

void persistFired() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < s_fired_n && i < FIRED_MAX; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["h"] = s_fired[i].hour;
    o["m"] = s_fired[i].minute;
    o["e"] = s_fired[i].epoch;
  }
  String out;
  serializeJson(doc, out);
  prefs::setFiredJson(out);
}

void persistStats() {
  JsonDocument doc;
  doc["d"] = s_day;
  doc["ok"] = s_ok_today;
  doc["fail"] = s_fail_today;
  doc["tot"] = s_tot;
  doc["sec"] = s_sec;
  String out;
  serializeJson(doc, out);
  prefs::setStatsJson(out);
}

void persistHopper() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < 4; i++) arr.add(s_used[i]);
  String out;
  serializeJson(doc, out);
  prefs::setHopperJson(out);
}

int32_t firedFind(uint8_t hour, uint8_t minute) {
  for (int i = 0; i < s_fired_n; i++) {
    if (s_fired[i].hour == hour && s_fired[i].minute == minute) return i;
  }
  return -1;
}

/** Roll the daily counters when the local date changed (or first use). */
void rollStatsIfNeeded() {
  const uint32_t today = nowDayStamp();
  if (today == 0 || today == s_day) return;
  s_day = today;
  s_ok_today = 0;
  s_fail_today = 0;
  // lifetime counters (s_tot / s_sec) survive the roll
  persistStats();
}

/** Most recent scheduled datetime (epoch) at/-before @p nowEpoch for an entry. */
time_t latestScheduledEpoch(const FeedTime &e, uint32_t nowEpoch) {
  const time_t now = (time_t)nowEpoch;
  struct tm lt;
  if (!localtime_r(&now, &lt)) return 0;
  for (int d = 0; d < 8; d++) { // walk back over weekend gaps in the mask
    struct tm cand = lt;
    cand.tm_mday -= d;
    cand.tm_hour = e.hour;
    cand.tm_min = e.minute;
    cand.tm_sec = 0;
    cand.tm_isdst = -1;
    const time_t te = mktime(&cand);
    if (te == (time_t)-1) return 0;
    if (te > (time_t)nowEpoch) continue;
    if (!entryCoversWday(e, cand.tm_wday)) continue; // mktime normalized mday
    return te;
  }
  return 0;
}

} // anonymous namespace

namespace feed_state {

void set_time_source(time_t (*fn)()) { s_time_fn = fn; }

void init() {
  // retry journal
  for (int i = 0; i < RETRY_SLOTS; i++) s_retry[i] = RetrySlot{};
  {
    const String j = prefs::getRetryJson();
    if (j.length()) {
      JsonDocument doc;
      if (deserializeJson(doc, j) == DeserializationError::Ok) {
        int i = 0;
        for (JsonObject o : doc.as<JsonArray>()) {
          if (i >= RETRY_SLOTS) break;
          RetrySlot &s = s_retry[i++];
          s.active = true;
          s.hour = (uint8_t)(o["h"] | 0);
          s.minute = (uint8_t)(o["m"] | 0);
          s.grams = (uint16_t)(o["g"] | 0);
          if (s.grams == 0) {
            // v1.10.0 migration: pre-1.10 journals stored seconds ("s" key).
            s.grams = catfishSecondsToGrams((uint8_t)(o["s"] | 3),
                                            prefs::getTankGps(1));
          }
          s.tankMask = (uint8_t)(o["t"] | 1);
          s.attempts = (uint8_t)(o["a"] | 1);
          s.dueEpoch = (uint32_t)(o["due"] | 0);
        }
      }
    }
    // A slot restored "in flight" (dueEpoch 0 = a retry was handed out but the
    // power died before it resolved) becomes due right after boot.
    for (int i = 0; i < RETRY_SLOTS; i++) {
      if (s_retry[i].active && s_retry[i].dueEpoch == 0)
        s_retry[i].dueEpoch = (uint32_t)nowTime();
    }
  }

  // fired map
  s_fired_n = 0;
  {
    const String j = prefs::getFiredJson();
    if (j.length()) {
      JsonDocument doc;
      if (deserializeJson(doc, j) == DeserializationError::Ok) {
        for (JsonObject o : doc.as<JsonArray>()) {
          if (s_fired_n >= FIRED_MAX) break;
          FiredSlot &f = s_fired[s_fired_n++];
          f.hour = (uint8_t)(o["h"] | 0);
          f.minute = (uint8_t)(o["m"] | 0);
          f.epoch = (uint32_t)(o["e"] | 0);
        }
      }
    }
  }

  // counters
  {
    const String j = prefs::getStatsJson();
    if (j.length()) {
      JsonDocument doc;
      if (deserializeJson(doc, j) == DeserializationError::Ok) {
        s_day = (uint32_t)(doc["d"] | 0);
        s_ok_today = (uint16_t)(doc["ok"] | 0);
        s_fail_today = (uint16_t)(doc["fail"] | 0);
        s_tot = (uint32_t)(doc["tot"] | 0);
        s_sec = (uint32_t)(doc["sec"] | 0);
      }
    }
  }
  rollStatsIfNeeded();

  // hopper
  {
    const String j = prefs::getHopperJson();
    if (j.length()) {
      JsonDocument doc;
      if (deserializeJson(doc, j) == DeserializationError::Ok) {
        int i = 0;
        for (JsonVariant v : doc.as<JsonArray>()) {
          if (i >= 4) break;
          s_used[i++] = (uint16_t)(v | 0);
        }
      }
    }
  }
}

// ------------------------------------------------------------ retry journal
bool journalArm(uint8_t hour, uint8_t minute, uint16_t grams,
                uint8_t tankMask) {
  const uint32_t now = (uint32_t)nowTime();
  RetrySlot *slot = nullptr;
  for (int i = 0; i < RETRY_SLOTS; i++) {
    if (s_retry[i].active && s_retry[i].hour == hour &&
        s_retry[i].minute == minute) {
      slot = &s_retry[i]; // refresh existing
      break;
    }
  }
  if (!slot) {
    for (int i = 0; i < RETRY_SLOTS; i++) {
      if (!s_retry[i].active) {
        slot = &s_retry[i];
        break;
      }
    }
  }
  if (!slot) return false;

  slot->active = true;
  slot->hour = hour;
  slot->minute = minute;
  slot->grams = grams;
  slot->tankMask = tankMask ? tankMask : 0x01;
  slot->attempts = slot->attempts ? slot->attempts : 1;
  const uint8_t bi = slot->attempts - 1;
  const uint32_t backoff =
      (bi < (int)(sizeof(RETRY_BACKOFF_S) / sizeof(RETRY_BACKOFF_S[0])))
          ? RETRY_BACKOFF_S[bi]
          : RETRY_BACKOFF_S[sizeof(RETRY_BACKOFF_S) / sizeof(RETRY_BACKOFF_S[0]) -
                            1];
  slot->dueEpoch = now + backoff;
  persistRetry();
  Serial.printf("[FEED] retry armed %02u:%02u (attempt %u, backoff %lus)\n",
                (unsigned)hour, (unsigned)minute, (unsigned)slot->attempts,
                (unsigned long)backoff);
  return true;
}

bool journalDue(uint32_t nowEpoch, RetryDue &out) {
  for (int i = 0; i < RETRY_SLOTS; i++) {
    RetrySlot &s = s_retry[i];
    if (!s.active || s.dueEpoch == 0) continue; // inactive or in flight
    if ((int32_t)(nowEpoch - s.dueEpoch) < 0) continue;
    out.hour = s.hour;
    out.minute = s.minute;
    out.grams = s.grams;
    out.tankMask = s.tankMask;
    s.dueEpoch = 0; // in flight until clear/fail resolves it
    persistRetry();
    Serial.printf("[FEED] retry due %02u:%02u (attempt %u)\n",
                  (unsigned)s.hour, (unsigned)s.minute, (unsigned)s.attempts);
    return true;
  }
  return false;
}

void journalClearFor(uint8_t hour, uint8_t minute) {
  bool changed = false;
  for (int i = 0; i < RETRY_SLOTS; i++) {
    if (s_retry[i].active && s_retry[i].hour == hour &&
        s_retry[i].minute == minute) {
      s_retry[i] = RetrySlot{};
      changed = true;
    }
  }
  if (changed) persistRetry();
}

bool journalFail(uint8_t hour, uint8_t minute) {
  for (int i = 0; i < RETRY_SLOTS; i++) {
    RetrySlot &s = s_retry[i];
    if (s.active && s.hour == hour && s.minute == minute) {
      s.attempts++;
      if (s.attempts >= RETRY_MAX_ATTEMPTS) {
        Serial.printf("[FEED] retry journal gave up on %02u:%02u after %u attempts\n",
                      (unsigned)hour, (unsigned)minute, (unsigned)s.attempts);
        s_retry[i] = RetrySlot{};
        persistRetry();
        return false;
      }
      const uint32_t now = (uint32_t)nowTime();
      s.dueEpoch = now + RETRY_BACKOFF_S[s.attempts - 1];
      persistRetry();
      return true;
    }
  }
  return false;
}

int journalCount() {
  int n = 0;
  for (int i = 0; i < RETRY_SLOTS; i++) n += s_retry[i].active ? 1 : 0;
  return n;
}

// ---------------------------------------------------------------- skip-next
void setSkipNext(uint8_t hour, uint8_t minute) {
  prefs::setSkipMinute((uint32_t)hour * 60 + minute);
  Serial.printf("[FEED] skip-next armed for %02u:%02u\n", (unsigned)hour,
                (unsigned)minute);
}

bool consumeSkip(uint8_t hour, uint8_t minute) {
  const uint32_t want = (uint32_t)hour * 60 + minute;
  if (prefs::getSkipMinute() != want) return false;
  prefs::setSkipMinute(0xFFFF);
  return true;
}

// ------------------------------------------------------------------ catch-up
int scanCatchup(const FeedTime *entries, int count, uint32_t nowEpoch,
                uint8_t windowMin, CatchupDue *out, int maxOut) {
  if (!entries || count <= 0 || !out || maxOut <= 0) return 0;
  if (windowMin == 0) return 0; // disabled

  int n = 0;
  for (int i = 0; i < count && n < maxOut; i++) {
    const FeedTime &e = entries[i];
    if (!e.enabled) continue;
    const time_t sched = latestScheduledEpoch(e, nowEpoch);
    if (sched == 0) continue;
    if ((int32_t)(nowEpoch - (uint32_t)sched) > (int32_t)windowMin * 60) continue;
    const int32_t fi = firedFind(e.hour, e.minute);
    const uint32_t lastFired = (fi >= 0) ? s_fired[fi].epoch : 0;
    if ((uint32_t)sched <= lastFired) continue; // already fired for this slot
    markFired(e.hour, e.minute, nowEpoch); // mark FIRST (crash-safe)
    CatchupDue &d = out[n++];
    d.hour = e.hour;
    d.minute = e.minute;
    d.grams = e.grams;
    d.tankMask = e.tanks;
    Serial.printf("[FEED] catch-up due %02u:%02u (missed %ld s ago)\n",
                  (unsigned)e.hour, (unsigned)e.minute,
                  (unsigned long)(nowEpoch - (uint32_t)sched));
  }
  return n;
}

void markFired(uint8_t hour, uint8_t minute, uint32_t epoch) {
  int32_t fi = firedFind(hour, minute);
  if (fi < 0) {
    if (s_fired_n < FIRED_MAX) {
      fi = s_fired_n++;
    } else {
      // ring is full: overwrite the oldest (least useful) entry
      int oldest = 0;
      for (int i = 1; i < FIRED_MAX; i++) {
        if (s_fired[i].epoch < s_fired[oldest].epoch) oldest = i;
      }
      fi = oldest;
    }
    s_fired[fi].hour = hour;
    s_fired[fi].minute = minute;
  }
  s_fired[fi].epoch = epoch;
  persistFired();
}

void pruneFired(const FeedTime *entries, int count) {
  bool changed = false;
  for (int i = s_fired_n - 1; i >= 0; i--) {
    bool found = false;
    for (int j = 0; j < count; j++) {
      if (entries[j].hour == s_fired[i].hour &&
          entries[j].minute == s_fired[i].minute) {
        found = true;
        break;
      }
    }
    if (!found) {
      s_fired[i] = s_fired[s_fired_n - 1];
      s_fired_n--;
      changed = true;
    }
  }
  if (changed) persistFired();
}

// ------------------------------------------------------------------ counters
void onFeedResult(bool ok, uint8_t seconds) {
  rollStatsIfNeeded();
  if (ok) {
    s_ok_today++;
    s_tot++;
    s_sec += seconds;
  } else {
    s_fail_today++;
  }
  persistStats();
}

uint16_t feedsOkToday() { return s_ok_today; }
uint16_t feedsFailToday() { return s_fail_today; }
uint32_t totalFeeds() { return s_tot; }
uint32_t totalSeconds() { return s_sec; }
uint32_t statsDay() { return s_day; }

// -------------------------------------------------------------------- guards
GuardResult checkFeed(FeedSource src, uint32_t nowEpoch) {
  GuardResult g; // default allowed
  if (src == SRC_TOUCH) return g; // a human pressed the button on the unit

  if (prefs::getVacation()) {
    g.allowed = false;
    g.reason = "vacation";
    return g;
  }
  const uint8_t gap = prefs::getMinGapMin();
  if (gap > 0) {
    const uint32_t last = prefs::getLastFedEpoch();
    if (last != 0 && nowEpoch < last + (uint32_t)gap * 60UL) {
      g.allowed = false;
      g.reason = "min gap";
      return g;
    }
  }
  const uint8_t maxDay = prefs::getMaxFeedsDay();
  if (maxDay > 0 && feedsOkToday() >= maxDay) {
    g.allowed = false;
    g.reason = "daily max";
    return g;
  }
  return g;
}

// -------------------------------------------------------------------- hopper
void hopperAddUsage(uint8_t tankMask, const uint16_t perTankGrams[4]) {
  bool changed = false;
  for (uint8_t t = 1; t <= 4; t++) {
    if (!(tankMask & (1u << (t - 1)))) continue;
    const uint16_t g = perTankGrams ? perTankGrams[t - 1] : 0;
    if (g == 0) continue;
    uint32_t used = (uint32_t)s_used[t - 1] + g;
    if (used > 60000) used = 60000; // saturate (refill resets)
    if (used != s_used[t - 1]) {
      s_used[t - 1] = (uint16_t)used;
      changed = true;
    }
  }
  if (changed) persistHopper();
}

void hopperRefill(uint8_t tank1) {
  if (tank1 == 0xFF) {
    for (int i = 0; i < 4; i++) s_used[i] = 0;
  } else if (tank1 >= 1 && tank1 <= 4) {
    s_used[tank1 - 1] = 0;
  } else {
    return;
  }
  persistHopper();
}

uint8_t hopperLevelPct(uint8_t tank1) {
  if (tank1 < 1 || tank1 > 4) return 255;
  const uint16_t cap = prefs::getTankHopperCap(tank1);
  if (cap == 0) return 255; // unknown
  if (s_used[tank1 - 1] == 0) return 100;
  if (s_used[tank1 - 1] >= cap) return 0;
  return (uint8_t)(100U - ((uint32_t)s_used[tank1 - 1] * 100UL) / cap);
}

uint16_t hopperUsedG(uint8_t tank1) {
  if (tank1 < 1 || tank1 > 4) return 0;
  return s_used[tank1 - 1];
}

bool hopperLowAny(uint8_t tankCount) {
  if (tankCount > 4) tankCount = 4;
  const uint8_t low = prefs::getHopperLowPct();
  for (uint8_t t = 1; t <= tankCount; t++) {
    const uint8_t pct = hopperLevelPct(t);
    if (pct != 255 && pct < low) return true;
  }
  return false;
}

// ---------------------------------------------------------------- history
void recordHistory(uint8_t tankMask, uint8_t seconds, uint16_t grams, bool ok,
                   uint8_t flag) {
  // shift-down insert: index 0 = newest (24 entries max, cheap on ESP32-S3)
  if (s_hist_n < HIST_MAX) s_hist_n++;
  for (int i = s_hist_n - 1; i > 0; i--) s_hist[i] = s_hist[i - 1];
  s_hist[0].epoch = (uint32_t)nowTime();
  s_hist[0].tankMask = tankMask;
  s_hist[0].seconds = seconds;
  s_hist[0].grams = grams;
  s_hist[0].ok = ok;
  s_hist[0].flag = flag;
  s_hist_dirty = true;
}

int historyCount() { return s_hist_n; }
const HistEntry *historyAll() { return s_hist; }
bool historyDirty() { return s_hist_dirty; }
void historyClearDirty() { s_hist_dirty = false; }

String historyJson(int maxEntries) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  const int n = (maxEntries > 0 && maxEntries < s_hist_n) ? maxEntries : s_hist_n;
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["e"] = s_hist[i].epoch;
    o["m"] = s_hist[i].tankMask;
    o["s"] = s_hist[i].seconds;
    o["g"] = s_hist[i].grams; // v1.10.0: portion (g)
    o["ok"] = s_hist[i].ok ? 1 : 0;
    o["f"] = s_hist[i].flag;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

} // namespace feed_state
