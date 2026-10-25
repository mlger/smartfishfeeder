/**
 * @file prefs.cpp
 * @brief Preferences (NVS) persistence wrapper, namespace "catfish".
 *
 * Write discipline: setters are called ONLY from loop() (single writer).
 * The network worker task reads getRelayToken() concurrently, so token access
 * is guarded by a spinlock critical section. All NVS write failures are logged.
 */
#include "prefs.h"
#include <Preferences.h>
#include <catfish_protocol.h>

namespace {

Preferences s_nvs;
bool s_loaded = false;

// Cached values (RAM), mirrored to NVS on every setter call.
char s_sched[1024] = {0};
char s_token[CATFISH_TOKEN_LEN] = CATFISH_DEFAULT_TOKEN;
char s_relay_ip[16] = {0};
uint8_t s_pulse = CATFISH_DEFAULT_PULSE_S;
uint32_t s_last_fed = 0;
bool s_bl_on = true;
bool s_hour12 = false;

// Multi-tank (Phase 5): 1-based tank ids, indexes below are tank-1.
uint8_t s_tank_count = 1;
char s_tank_name[CATFISH_MAX_TANKS][CATFISH_TANK_NAME_LEN] = {
    "Main", "Tank 2", "Tank 3", "Tank 4"};
char s_tank_tok[CATFISH_MAX_TANKS][CATFISH_TOKEN_LEN] = {{0}, {0}, {0}, {0}};
char s_tank_ip[CATFISH_MAX_TANKS][16] = {{0}, {0}, {0}, {0}};

// MQTT / Home Assistant (loop-context only access; no guard needed)
bool s_mq_en = false;
char s_mq_host[CATFISH_MQTT_HOST_LEN] = {0};
uint16_t s_mq_port = CATFISH_MQTT_PORT;
char s_mq_user[CATFISH_MQTT_USER_LEN] = {0};
char s_mq_pass[CATFISH_MQTT_PASS_LEN] = {0};
char s_mq_prefix[CATFISH_MQTT_PREFIX_LEN] = CATFISH_MQTT_PREFIX;

// Feeding state (v1.9.0): JSON blobs + policy scalars, all loop()-context only
char s_retry[256] = {0};
char s_fired[512] = {0};
char s_stats[192] = {0};
char s_hopper[192] = {0}; // v1.10.0: per-tank grams since refill (key "hopG")
uint32_t s_skip_min = 0xFFFF; // 0xFFFF = none
bool s_vacation = false;
uint8_t s_gap_min = 0;    // 0 = off
uint8_t s_max_day = 0;    // 0 = unlimited
uint8_t s_ctch_win = 10;  // minutes; 0 = disabled
uint8_t s_hop_low = 20;   // percent
uint16_t s_tank_gps[CATFISH_MAX_TANKS] = {0, 0, 0, 0}; // g/second per tank (0 = nominal)
// v1.10.0: hopper capacity in GRAMS (0 = unknown). v1.9.0 stored pulse-seconds;
// begin() migrates old values once to grams (x tank gps) and sets "hcapG".
uint16_t s_tank_hcap[CATFISH_MAX_TANKS] = {0, 0, 0, 0};
uint16_t s_def_grams = CATFISH_DEFAULT_GRAMS; // v1.10.0: default portion (g)

// Guards s_token: written by loop(), read by the net worker task.
portMUX_TYPE s_token_mux = portMUX_INITIALIZER_UNLOCKED;
// v1.8.2: guards the shared char-blob buffers (schedule JSON, tank names /
// tokens / IPs, MQTT config). Getters run on the LVGL task (overlay prefills)
// and the web task (/api/schedule), setters run on loop() — and setTankIp()
// is also called from the net worker (self-heal). Without the guard a reader
// could copy a buffer mid-strlcpy (transiently unterminated). Copy under the
// short critical section, build the String / do NVS work outside it.
portMUX_TYPE s_blob_mux = portMUX_INITIALIZER_UNLOCKED;

static String blobCopy(const char *src, size_t cap) {
  // v1.10.0: the stack buffer was a flat 1 KB regardless of the blob size;
  // size it to the real capacity instead — several getters run on the 6 KB
  // LVGL task (overlay prefills) and every KB of stack there counts.
  char buf[128];
  char *dst = buf;
  char *heap = nullptr;
  size_t n = cap;
  if (n > sizeof(buf)) {
    heap = (char *)malloc(n);
    if (!heap) return String(); // allocation failure: report empty, log-free
    dst = heap;
  } else {
    n = (cap < sizeof(buf)) ? cap : sizeof(buf);
  }
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(dst, src, n);
  portEXIT_CRITICAL(&s_blob_mux);
  String out(dst);
  free(heap);
  return out;
}

void nvsLogFail(const char *key) {
  Serial.printf("[SYS] WARN: NVS write failed for key '%s'\n", key);
}

} // anonymous namespace

namespace prefs {

bool begin() {
  if (!s_nvs.begin("catfish", false)) {
    Serial.println("[SYS] ERROR: NVS begin('catfish') failed; defaults in use");
    return false;
  }

  String v;
  v = s_nvs.getString("sched", "");
  strlcpy(s_sched, v.c_str(), sizeof(s_sched));

  v = s_nvs.getString("token", CATFISH_DEFAULT_TOKEN);
  strlcpy(s_token, v.c_str(), sizeof(s_token));

  v = s_nvs.getString("relayIp", "");
  strlcpy(s_relay_ip, v.c_str(), sizeof(s_relay_ip));

  s_pulse = s_nvs.getUChar("pulse", CATFISH_DEFAULT_PULSE_S);
  if (s_pulse < 1 || s_pulse > CATFISH_PULSE_S_LIMIT)
    s_pulse = CATFISH_DEFAULT_PULSE_S;

  s_last_fed = s_nvs.getULong("lastFed", 0);
  s_bl_on = s_nvs.getBool("blOn", true);
  s_hour12 = s_nvs.getBool("fmt12", false);

  s_mq_en = s_nvs.getBool("mqEn", false);
  v = s_nvs.getString("mqHost", "");
  strlcpy(s_mq_host, v.c_str(), sizeof(s_mq_host));
  s_mq_port = s_nvs.getUShort("mqPort", CATFISH_MQTT_PORT);
  if (s_mq_port == 0) s_mq_port = CATFISH_MQTT_PORT;
  v = s_nvs.getString("mqUser", "");
  strlcpy(s_mq_user, v.c_str(), sizeof(s_mq_user));
  v = s_nvs.getString("mqPass", "");
  strlcpy(s_mq_pass, v.c_str(), sizeof(s_mq_pass));
  v = s_nvs.getString("mqPre", CATFISH_MQTT_PREFIX);
  strlcpy(s_mq_prefix, v.c_str(), sizeof(s_mq_prefix));
  if (!s_mq_prefix[0]) strlcpy(s_mq_prefix, CATFISH_MQTT_PREFIX, sizeof(s_mq_prefix));

  // ---- multi-tank (Phase 5) ----
  s_tank_count = s_nvs.getUChar("tankN", 1);
  if (s_tank_count < 1 || s_tank_count > CATFISH_MAX_TANKS) s_tank_count = 1;
  for (int i = 0; i < CATFISH_MAX_TANKS; i++) {
    char key[8];
    snprintf(key, sizeof(key), "tName%d", i + 1);
    v = s_nvs.getString(key, "");
    if (v.length()) strlcpy(s_tank_name[i], v.c_str(), sizeof(s_tank_name[i]));
    snprintf(key, sizeof(key), "tTok%d", i + 1);
    v = s_nvs.getString(key, "");
    strlcpy(s_tank_tok[i], v.c_str(), sizeof(s_tank_tok[i]));
    snprintf(key, sizeof(key), "tIp%d", i + 1);
    v = s_nvs.getString(key, "");
    strlcpy(s_tank_ip[i], v.c_str(), sizeof(s_tank_ip[i]));
  }
  // Migration from single-relay installs: tank 1 inherits the legacy IP slot
  // until its own key exists. (setTankIp(1) keeps "relayIp" mirrored, so this
  // only fires once after the upgrade.)
  if (!s_tank_ip[0][0] && s_relay_ip[0]) {
    strlcpy(s_tank_ip[0], s_relay_ip, sizeof(s_tank_ip[0]));
    if (s_nvs.putString("tIp1", s_tank_ip[0]) == 0) nvsLogFail("tIp1");
  }

  // ---- feeding state + policy (v1.9.0) ----
  v = s_nvs.getString("retry", "");
  strlcpy(s_retry, v.c_str(), sizeof(s_retry));
  v = s_nvs.getString("fired", "");
  strlcpy(s_fired, v.c_str(), sizeof(s_fired));
  v = s_nvs.getString("stats", "");
  strlcpy(s_stats, v.c_str(), sizeof(s_stats));
  v = s_nvs.getString("hopG", ""); // v1.10.0: grams; the old sec-based "hopper"
  strlcpy(s_hopper, v.c_str(), sizeof(s_hopper)); // key is intentionally ignored
  s_skip_min = s_nvs.getULong("skipMin", 0xFFFF);
  s_vacation = s_nvs.getBool("vacation", false);
  s_gap_min = s_nvs.getUChar("gapMin", 0);
  s_max_day = s_nvs.getUChar("maxDay", 0);
  s_ctch_win = s_nvs.getUChar("ctchWin", 10);
  s_hop_low = s_nvs.getUChar("hopLow", 20);
  for (int i = 0; i < CATFISH_MAX_TANKS; i++) {
    char key[8];
    snprintf(key, sizeof(key), "gps%d", i + 1);
    s_tank_gps[i] = s_nvs.getUShort(key, 0);
    snprintf(key, sizeof(key), "hcap%d", i + 1);
    s_tank_hcap[i] = s_nvs.getUShort(key, 0);
  }
  // v1.10.0 one-time migration: v1.9.0 stored hopper caps in pulse-seconds;
  // grams = seconds x tank gps (nominal 5 g/s when unset). The flag key makes
  // the conversion exactly-once so fresh installs are untouched.
  if (!s_nvs.getBool("hcapG", false)) {
    for (int i = 0; i < CATFISH_MAX_TANKS; i++) {
      if (s_tank_hcap[i] == 0) continue;
      uint16_t gps = s_tank_gps[i] ? s_tank_gps[i] : CATFISH_DEFAULT_GPS;
      uint32_t g = (uint32_t)s_tank_hcap[i] * gps;
      if (g > CATFISH_MAX_GRAMS) g = CATFISH_MAX_GRAMS;
      s_tank_hcap[i] = (uint16_t)g;
      char key[8];
      snprintf(key, sizeof(key), "hcap%d", i + 1);
      if (s_nvs.putUShort(key, s_tank_hcap[i]) == 0) nvsLogFail(key);
    }
    if (!s_nvs.putBool("hcapG", true)) nvsLogFail("hcapG");
  }
  s_def_grams = s_nvs.getUShort("defG", CATFISH_DEFAULT_GRAMS);
  if (s_def_grams < 1 || s_def_grams > CATFISH_MAX_GRAMS)
    s_def_grams = CATFISH_DEFAULT_GRAMS;
  if (s_hop_low < 5 || s_hop_low > 90) s_hop_low = 20;

  s_loaded = true;
  Serial.printf("[SYS] prefs loaded from NVS (namespace 'catfish', %u tank%s)\n",
                (unsigned)s_tank_count, (s_tank_count == 1) ? "" : "s");
  return true;
}

bool isLoaded() { return s_loaded; }

String getScheduleJson() { return blobCopy(s_sched, sizeof(s_sched)); }

bool setScheduleJson(const String &json) {
  // Never truncate silently: an oversized blob would corrupt the schedule.
  if (json.length() >= sizeof(s_sched)) {
    Serial.printf("[SYS] WARN: schedule JSON too large (%u B, cap %u B)\n",
                  (unsigned)json.length(), (unsigned)sizeof(s_sched));
    return false;
  }
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_sched, json.c_str(), sizeof(s_sched));
  portEXIT_CRITICAL(&s_blob_mux);
  if (s_loaded && s_nvs.putString("sched", json) == 0) nvsLogFail("sched");
  return true;
}

String getRelayToken() {
  char buf[CATFISH_TOKEN_LEN];
  portENTER_CRITICAL(&s_token_mux);
  strlcpy(buf, s_token, sizeof(buf));
  portEXIT_CRITICAL(&s_token_mux);
  return String(buf);
}

void setRelayToken(const String &token) {
  portENTER_CRITICAL(&s_token_mux);
  strlcpy(s_token, token.c_str(), sizeof(s_token));
  portEXIT_CRITICAL(&s_token_mux);
  if (s_loaded && s_nvs.putString("token", token) == 0) nvsLogFail("token");
}

// ---- legacy single-relay IP slot: now an alias for tank 1 (multi-tank) ----
String getRelayIp() { return getTankIp(1); }

void setRelayIp(const String &ip) { setTankIp(1, ip); }

// ---------------------------------------------------------- multi-tank (P5)
uint8_t getTankCount() { return s_tank_count; }

void setTankCount(uint8_t count) {
  if (count < 1) count = 1;
  if (count > CATFISH_MAX_TANKS) count = CATFISH_MAX_TANKS;
  s_tank_count = count;
  if (s_loaded && s_nvs.putUChar("tankN", s_tank_count) == 0) nvsLogFail("tankN");
}

static int tankIdx(uint8_t tank1) {
  const int i = (int)tank1 - 1;
  if (i < 0 || i >= CATFISH_MAX_TANKS) return -1;
  return i;
}

String getTankName(uint8_t tank1) {
  const int i = tankIdx(tank1);
  if (i < 0) return String("");
  return blobCopy(s_tank_name[i], sizeof(s_tank_name[i]));
}

void setTankName(uint8_t tank1, const String &name) {
  const int i = tankIdx(tank1);
  if (i < 0) return;
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_tank_name[i], name.c_str(), sizeof(s_tank_name[i]));
  if (!s_tank_name[i][0])
    snprintf(s_tank_name[i], sizeof(s_tank_name[i]), "Tank %u", (unsigned)tank1);
  portEXIT_CRITICAL(&s_blob_mux);
  char key[8];
  snprintf(key, sizeof(key), "tName%d", i + 1);
  if (s_loaded && s_nvs.putString(key, s_tank_name[i]) == 0) nvsLogFail(key);
}

String getTankToken(uint8_t tank1) {
  const int i = tankIdx(tank1);
  if (i < 0) return String("");
  return blobCopy(s_tank_tok[i], sizeof(s_tank_tok[i]));
}

void setTankToken(uint8_t tank1, const String &token) {
  const int i = tankIdx(tank1);
  if (i < 0) return;
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_tank_tok[i], token.c_str(), sizeof(s_tank_tok[i]));
  portEXIT_CRITICAL(&s_blob_mux);
  char key[8];
  snprintf(key, sizeof(key), "tTok%d", i + 1);
  if (s_loaded && s_nvs.putString(key, s_tank_tok[i]) == 0) nvsLogFail(key);
}

String getTankIp(uint8_t tank1) {
  const int i = tankIdx(tank1);
  if (i < 0) return String("");
  return blobCopy(s_tank_ip[i], sizeof(s_tank_ip[i]));
}

void setTankIp(uint8_t tank1, const String &ip) {
  const int i = tankIdx(tank1);
  if (i < 0) return;
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_tank_ip[i], ip.c_str(), sizeof(s_tank_ip[i]));
  portEXIT_CRITICAL(&s_blob_mux);
  char key[8];
  snprintf(key, sizeof(key), "tIp%d", i + 1);
  if (s_loaded && s_nvs.putString(key, s_tank_ip[i]) == 0) nvsLogFail(key);
  if (i == 0) {
    // keep the legacy slot mirrored for downgrade + older tooling
    strlcpy(s_relay_ip, ip.c_str(), sizeof(s_relay_ip));
    if (s_loaded && s_nvs.putString("relayIp", ip) == 0) nvsLogFail("relayIp");
  }
}

String effectiveTankToken(uint8_t tank1) {
  const int i = tankIdx(tank1);
  if (i >= 0 && s_tank_tok[i][0]) return String(s_tank_tok[i]);
  return getRelayToken(); // global pairing token
}

uint8_t getDefaultPulse() { return s_pulse; }

void setDefaultPulse(uint8_t seconds) {
  if (seconds < 1) seconds = 1;
  if (seconds > CATFISH_PULSE_S_LIMIT) seconds = CATFISH_PULSE_S_LIMIT;
  s_pulse = seconds;
  if (s_loaded && s_nvs.putUChar("pulse", s_pulse) == 0) nvsLogFail("pulse");
}

// -------------------------------------------------- v1.10.0: portion (grams)
uint16_t getDefaultGrams() { return s_def_grams; }

void setDefaultGrams(uint16_t grams) {
  s_def_grams = catfishClampGrams(grams);
  if (s_loaded && s_nvs.putUShort("defG", s_def_grams) == 0)
    nvsLogFail("defG");
}

uint32_t getLastFedEpoch() { return s_last_fed; }

void setLastFedEpoch(uint32_t epoch) {
  s_last_fed = epoch;
  if (s_loaded && s_nvs.putULong("lastFed", epoch) == 0) nvsLogFail("lastFed");
}

// ----------------------------------------------- feeding state blobs (v1.9.0)
String getRetryJson() { return blobCopy(s_retry, sizeof(s_retry)); }

bool setRetryJson(const String &json) {
  if (json.length() >= sizeof(s_retry)) return false;
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_retry, json.c_str(), sizeof(s_retry));
  portEXIT_CRITICAL(&s_blob_mux);
  if (s_loaded && s_nvs.putString("retry", json) == 0) nvsLogFail("retry");
  return true;
}

String getFiredJson() { return blobCopy(s_fired, sizeof(s_fired)); }

bool setFiredJson(const String &json) {
  if (json.length() >= sizeof(s_fired)) return false;
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_fired, json.c_str(), sizeof(s_fired));
  portEXIT_CRITICAL(&s_blob_mux);
  if (s_loaded && s_nvs.putString("fired", json) == 0) nvsLogFail("fired");
  return true;
}

String getStatsJson() { return blobCopy(s_stats, sizeof(s_stats)); }

bool setStatsJson(const String &json) {
  if (json.length() >= sizeof(s_stats)) return false;
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_stats, json.c_str(), sizeof(s_stats));
  portEXIT_CRITICAL(&s_blob_mux);
  if (s_loaded && s_nvs.putString("stats", json) == 0) nvsLogFail("stats");
  return true;
}

String getHopperJson() { return blobCopy(s_hopper, sizeof(s_hopper)); }

bool setHopperJson(const String &json) {
  if (json.length() >= sizeof(s_hopper)) return false;
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_hopper, json.c_str(), sizeof(s_hopper));
  portEXIT_CRITICAL(&s_blob_mux);
  if (s_loaded && s_nvs.putString("hopG", json) == 0) nvsLogFail("hopG");
  return true;
}

uint32_t getSkipMinute() { return s_skip_min; }

void setSkipMinute(uint32_t minOfDay) {
  if (minOfDay > 24 * 60) minOfDay = 0xFFFF; // anything past midnight = none
  s_skip_min = minOfDay;
  if (s_loaded && s_nvs.putULong("skipMin", s_skip_min) == 0) nvsLogFail("skipMin");
}

// ------------------------------------------------------ feeding policy (v1.9.0)
bool getVacation() { return s_vacation; }

void setVacation(bool on) {
  s_vacation = on;
  if (s_loaded && !s_nvs.putBool("vacation", on)) nvsLogFail("vacation");
}

uint8_t getMinGapMin() { return s_gap_min; }

void setMinGapMin(uint8_t minutes) {
  s_gap_min = minutes;
  if (s_loaded && s_nvs.putUChar("gapMin", s_gap_min) == 0) nvsLogFail("gapMin");
}

uint8_t getMaxFeedsDay() { return s_max_day; }

void setMaxFeedsDay(uint8_t maxFeeds) {
  s_max_day = maxFeeds;
  if (s_loaded && s_nvs.putUChar("maxDay", s_max_day) == 0) nvsLogFail("maxDay");
}

uint8_t getCatchupWin() { return s_ctch_win; }

void setCatchupWin(uint8_t minutes) {
  if (minutes > 120) minutes = 120;
  s_ctch_win = minutes;
  if (s_loaded && s_nvs.putUChar("ctchWin", s_ctch_win) == 0) nvsLogFail("ctchWin");
}

uint8_t getHopperLowPct() { return s_hop_low; }

void setHopperLowPct(uint8_t pct) {
  if (pct < 5) pct = 5;
  if (pct > 90) pct = 90;
  s_hop_low = pct;
  if (s_loaded && s_nvs.putUChar("hopLow", s_hop_low) == 0) nvsLogFail("hopLow");
}

uint16_t getTankGps(uint8_t tank1) {
  const int i = tankIdx(tank1);
  return (i < 0) ? 0 : s_tank_gps[i];
}

void setTankGps(uint8_t tank1, uint16_t gps) {
  const int i = tankIdx(tank1);
  if (i < 0) return;
  s_tank_gps[i] = gps;
  char key[8];
  snprintf(key, sizeof(key), "gps%d", i + 1);
  if (s_loaded && s_nvs.putUShort(key, gps) == 0) nvsLogFail(key);
}

uint16_t getTankHopperCap(uint8_t tank1) {
  const int i = tankIdx(tank1);
  return (i < 0) ? 0 : s_tank_hcap[i];
}

void setTankHopperCap(uint8_t tank1, uint16_t capG) {
  const int i = tankIdx(tank1);
  if (i < 0) return;
  s_tank_hcap[i] = capG;
  char key[8];
  snprintf(key, sizeof(key), "hcap%d", i + 1);
  if (s_loaded && s_nvs.putUShort(key, capG) == 0) nvsLogFail(key);
}

bool getBacklightOn() { return s_bl_on; }

void setBacklightOn(bool on) {
  s_bl_on = on;
  if (s_loaded && !s_nvs.putBool("blOn", on)) nvsLogFail("blOn");
}

bool getHour12() { return s_hour12; }

void setHour12(bool h12) {
  s_hour12 = h12;
  if (s_loaded && !s_nvs.putBool("fmt12", h12)) nvsLogFail("fmt12");
}

// ---------------------------------------------------------------- MQTT
bool getMqttEnabled() { return s_mq_en; }

void setMqttEnabled(bool on) {
  s_mq_en = on;
  if (s_loaded && !s_nvs.putBool("mqEn", on)) nvsLogFail("mqEn");
}

String getMqttHost() { return blobCopy(s_mq_host, sizeof(s_mq_host)); }

void setMqttHost(const String &host) {
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_mq_host, host.c_str(), sizeof(s_mq_host));
  portEXIT_CRITICAL(&s_blob_mux);
  if (s_loaded && s_nvs.putString("mqHost", host) == 0) nvsLogFail("mqHost");
}

uint16_t getMqttPort() { return s_mq_port; }

void setMqttPort(uint16_t port) {
  if (port == 0) port = CATFISH_MQTT_PORT;
  s_mq_port = port;
  if (s_loaded && s_nvs.putUShort("mqPort", port) == 0) nvsLogFail("mqPort");
}

String getMqttUser() { return blobCopy(s_mq_user, sizeof(s_mq_user)); }

void setMqttUser(const String &user) {
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_mq_user, user.c_str(), sizeof(s_mq_user));
  portEXIT_CRITICAL(&s_blob_mux);
  if (s_loaded && s_nvs.putString("mqUser", user) == 0) nvsLogFail("mqUser");
}

String getMqttPass() { return blobCopy(s_mq_pass, sizeof(s_mq_pass)); }

void setMqttPass(const String &pass) {
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_mq_pass, pass.c_str(), sizeof(s_mq_pass));
  portEXIT_CRITICAL(&s_blob_mux);
  if (s_loaded && s_nvs.putString("mqPass", pass) == 0) nvsLogFail("mqPass");
}

String getMqttPrefix() { return blobCopy(s_mq_prefix, sizeof(s_mq_prefix)); }

void setMqttPrefix(const String &prefix) {
  portENTER_CRITICAL(&s_blob_mux);
  strlcpy(s_mq_prefix, prefix.c_str(), sizeof(s_mq_prefix));
  if (!s_mq_prefix[0]) strlcpy(s_mq_prefix, CATFISH_MQTT_PREFIX, sizeof(s_mq_prefix));
  portEXIT_CRITICAL(&s_blob_mux);
  if (s_loaded && s_nvs.putString("mqPre", s_mq_prefix) == 0) nvsLogFail("mqPre");
}

} // namespace prefs
