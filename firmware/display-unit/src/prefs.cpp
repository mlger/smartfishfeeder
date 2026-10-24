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

  s_loaded = true;
  Serial.println("[SYS] prefs loaded from NVS (namespace 'catfish')");
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

String getRelayIp() { return String(s_relay_ip); }

void setRelayIp(const String &ip) {
  strlcpy(s_relay_ip, ip.c_str(), sizeof(s_relay_ip));
  if (s_loaded && s_nvs.putString("relayIp", ip) == 0) nvsLogFail("relayIp");
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

} // namespace prefs
