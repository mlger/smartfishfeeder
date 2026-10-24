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

// Guards s_token: written by loop(), read by the net worker task.
portMUX_TYPE s_token_mux = portMUX_INITIALIZER_UNLOCKED;

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

  s_loaded = true;
  Serial.printf("[SYS] prefs loaded from NVS (namespace 'catfish', %u tank%s)\n",
                (unsigned)s_tank_count, (s_tank_count == 1) ? "" : "s");
  return true;
}

bool isLoaded() { return s_loaded; }

String getScheduleJson() { return String(s_sched); }

bool setScheduleJson(const String &json) {
  // Never truncate silently: an oversized blob would corrupt the schedule.
  if (json.length() >= sizeof(s_sched)) {
    Serial.printf("[SYS] WARN: schedule JSON too large (%u B, cap %u B)\n",
                  (unsigned)json.length(), (unsigned)sizeof(s_sched));
    return false;
  }
  strlcpy(s_sched, json.c_str(), sizeof(s_sched));
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
  return String(s_tank_name[i]);
}

void setTankName(uint8_t tank1, const String &name) {
  const int i = tankIdx(tank1);
  if (i < 0) return;
  strlcpy(s_tank_name[i], name.c_str(), sizeof(s_tank_name[i]));
  if (!s_tank_name[i][0])
    snprintf(s_tank_name[i], sizeof(s_tank_name[i]), "Tank %u", (unsigned)tank1);
  char key[8];
  snprintf(key, sizeof(key), "tName%d", i + 1);
  if (s_loaded && s_nvs.putString(key, s_tank_name[i]) == 0) nvsLogFail(key);
}

String getTankToken(uint8_t tank1) {
  const int i = tankIdx(tank1);
  if (i < 0) return String("");
  return String(s_tank_tok[i]);
}

void setTankToken(uint8_t tank1, const String &token) {
  const int i = tankIdx(tank1);
  if (i < 0) return;
  strlcpy(s_tank_tok[i], token.c_str(), sizeof(s_tank_tok[i]));
  char key[8];
  snprintf(key, sizeof(key), "tTok%d", i + 1);
  if (s_loaded && s_nvs.putString(key, s_tank_tok[i]) == 0) nvsLogFail(key);
}

String getTankIp(uint8_t tank1) {
  const int i = tankIdx(tank1);
  if (i < 0) return String("");
  return String(s_tank_ip[i]);
}

void setTankIp(uint8_t tank1, const String &ip) {
  const int i = tankIdx(tank1);
  if (i < 0) return;
  strlcpy(s_tank_ip[i], ip.c_str(), sizeof(s_tank_ip[i]));
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

uint32_t getLastFedEpoch() { return s_last_fed; }

void setLastFedEpoch(uint32_t epoch) {
  s_last_fed = epoch;
  if (s_loaded && s_nvs.putULong("lastFed", epoch) == 0) nvsLogFail("lastFed");
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

String getMqttHost() { return String(s_mq_host); }

void setMqttHost(const String &host) {
  strlcpy(s_mq_host, host.c_str(), sizeof(s_mq_host));
  if (s_loaded && s_nvs.putString("mqHost", host) == 0) nvsLogFail("mqHost");
}

uint16_t getMqttPort() { return s_mq_port; }

void setMqttPort(uint16_t port) {
  if (port == 0) port = CATFISH_MQTT_PORT;
  s_mq_port = port;
  if (s_loaded && s_nvs.putUShort("mqPort", port) == 0) nvsLogFail("mqPort");
}

String getMqttUser() { return String(s_mq_user); }

void setMqttUser(const String &user) {
  strlcpy(s_mq_user, user.c_str(), sizeof(s_mq_user));
  if (s_loaded && s_nvs.putString("mqUser", user) == 0) nvsLogFail("mqUser");
}

String getMqttPass() { return String(s_mq_pass); }

void setMqttPass(const String &pass) {
  strlcpy(s_mq_pass, pass.c_str(), sizeof(s_mq_pass));
  if (s_loaded && s_nvs.putString("mqPass", pass) == 0) nvsLogFail("mqPass");
}

String getMqttPrefix() { return String(s_mq_prefix); }

void setMqttPrefix(const String &prefix) {
  strlcpy(s_mq_prefix, prefix.c_str(), sizeof(s_mq_prefix));
  if (!s_mq_prefix[0]) strlcpy(s_mq_prefix, CATFISH_MQTT_PREFIX, sizeof(s_mq_prefix));
  if (s_loaded && s_nvs.putString("mqPre", s_mq_prefix) == 0) nvsLogFail("mqPre");
}

} // namespace prefs
