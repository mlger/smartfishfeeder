/**
 * @file prefs.cpp
 * @brief Preferences (NVS) persistence wrapper, namespace "catfish".
 */
#include "prefs.h"
#include <Preferences.h>

namespace {

Preferences s_nvs;
bool s_loaded = false;

// Cached values (RAM), mirrored to NVS on every setter call.
char s_sched[1024] = {0};
char s_token[33] = "catfish";
char s_relay_ip[16] = {0};
uint8_t s_pulse = 3;
uint32_t s_last_fed = 0;

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

  v = s_nvs.getString("token", "catfish");
  strlcpy(s_token, v.c_str(), sizeof(s_token));

  v = s_nvs.getString("relayIp", "");
  strlcpy(s_relay_ip, v.c_str(), sizeof(s_relay_ip));

  s_pulse = s_nvs.getUChar("pulse", 3);
  if (s_pulse < 1 || s_pulse > 30) s_pulse = 3;

  s_last_fed = s_nvs.getULong("lastFed", 0);

  s_loaded = true;
  Serial.println("[SYS] prefs loaded from NVS (namespace 'catfish')");
  return true;
}

bool isLoaded() { return s_loaded; }

String getScheduleJson() { return String(s_sched); }

void setScheduleJson(const String &json) {
  strlcpy(s_sched, json.c_str(), sizeof(s_sched));
  if (s_loaded) s_nvs.putString("sched", json);
}

String getRelayToken() { return String(s_token); }

String getRelayIp() { return String(s_relay_ip); }

void setRelayIp(const String &ip) {
  strlcpy(s_relay_ip, ip.c_str(), sizeof(s_relay_ip));
  if (s_loaded) s_nvs.putString("relayIp", ip);
}

uint8_t getDefaultPulse() { return s_pulse; }

void setDefaultPulse(uint8_t seconds) {
  if (seconds < 1) seconds = 1;
  if (seconds > 30) seconds = 30;
  s_pulse = seconds;
  if (s_loaded) s_nvs.putUChar("pulse", s_pulse);
}

uint32_t getLastFedEpoch() { return s_last_fed; }

void setLastFedEpoch(uint32_t epoch) {
  s_last_fed = epoch;
  if (s_loaded) s_nvs.putULong("lastFed", epoch);
}

} // namespace prefs
