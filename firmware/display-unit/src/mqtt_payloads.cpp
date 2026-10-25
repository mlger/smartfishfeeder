/**
 * @file mqtt_payloads.cpp
 * @brief Pure MQTT topic/payload builders for Home Assistant auto-discovery.
 *
 * Keep this file free of Arduino/ESP includes beyond Arduino.h so it stays
 * testable in the [env:native] host environment (see test/test_mqtt_payloads).
 */
#include "mqtt_payloads.h"

#include <ArduinoJson.h>
#include <catfish_protocol.h>
#include <time.h>

namespace mqtt_payloads {

namespace {

/** Shared device block for every discovery config (one HA device). */
void addDevice(JsonDocument &doc) {
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"][0] = "catfish-display";
  dev["name"] = "Catfish Feeder";
  dev["manufacturer"] = "Catfish Feeder project";
  dev["model"] = "Display Unit (ESP32-S3)";
  dev["sw_version"] = CATFISH_APP_VERSION;
}

/** Availability block pointing at the single retained availability topic. */
void addAvailability(JsonDocument &doc, const String &prefix) {
  doc["availability_topic"] = availabilityTopic(prefix);
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
}

String serialize(const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  return out;
}

} // anonymous namespace

// ---------------------------------------------------------------- topics
String makeTopic(const String &prefix, const char *leaf) {
  String p = prefix.length() ? prefix : String(CATFISH_MQTT_PREFIX);
  return p + "/" + leaf;
}

String availabilityTopic(const String &prefix) { return makeTopic(prefix, "availability"); }
String feedCmdTopic(const String &prefix) { return makeTopic(prefix, "feed/cmd"); }
String pulseSetTopic(const String &prefix) { return makeTopic(prefix, "pulse/set"); }
String pulseStateTopic(const String &prefix) { return makeTopic(prefix, "pulse/state"); }
String feedingTopic(const String &prefix) { return makeTopic(prefix, "feeding"); }
String relayOnlineTopic(const String &prefix) { return makeTopic(prefix, "relay/online"); }
String lastFeedTopic(const String &prefix) { return makeTopic(prefix, "last_feed"); }
String nextFeedTopic(const String &prefix) { return makeTopic(prefix, "next_feed"); }

// -------------------------------------------------- v1.9.0 state topics
String pausedTopic(const String &prefix) { return makeTopic(prefix, "paused"); }
String feedsTodayTopic(const String &prefix) { return makeTopic(prefix, "feeds_today"); }
String failsTodayTopic(const String &prefix) { return makeTopic(prefix, "fails_today"); }

String tankLastFeedTopic(const String &prefix, uint8_t tank1) {
  String p = prefix.length() ? prefix : String(CATFISH_MQTT_PREFIX);
  char leaf[24];
  snprintf(leaf, sizeof(leaf), "/tank%u/last_feed", (unsigned)tank1);
  return p + leaf;
}

String tankSecondsTopic(const String &prefix, uint8_t tank1) {
  String p = prefix.length() ? prefix : String(CATFISH_MQTT_PREFIX);
  char leaf[24];
  snprintf(leaf, sizeof(leaf), "/tank%u/seconds", (unsigned)tank1);
  return p + leaf;
}

String tankFoodTopic(const String &prefix, uint8_t tank1) {
  String p = prefix.length() ? prefix : String(CATFISH_MQTT_PREFIX);
  char leaf[20];
  snprintf(leaf, sizeof(leaf), "/tank%u/food", (unsigned)tank1);
  return p + leaf;
}

// ------------------------------------------------------- discovery configs
String discoveryFeedButton(const String &prefix) {
  JsonDocument doc;
  doc["name"] = "Feed now";
  doc["command_topic"] = feedCmdTopic(prefix);
  doc["payload_press"] = "ON";
  doc["icon"] = "mdi:fish-food";
  doc["uniq_id"] = "catfish_feed_now";
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

String discoveryPulseNumber(const String &prefix) {
  JsonDocument doc;
  doc["name"] = "Default pulse";
  doc["state_topic"] = pulseStateTopic(prefix);
  doc["command_topic"] = pulseSetTopic(prefix);
  doc["min"] = 1;
  doc["max"] = CATFISH_PULSE_S_LIMIT;
  doc["step"] = 1;
  doc["mode"] = "box";
  doc["unit_of_measurement"] = "s";
  doc["icon"] = "mdi:timer";
  doc["uniq_id"] = "catfish_pulse";
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

String discoveryRelayOnline(const String &prefix) {
  JsonDocument doc;
  doc["name"] = "Relay online";
  doc["state_topic"] = relayOnlineTopic(prefix);
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["device_class"] = "connectivity";
  doc["uniq_id"] = "catfish_relay_online";
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

String discoveryFeeding(const String &prefix) {
  JsonDocument doc;
  doc["name"] = "Feeding";
  doc["state_topic"] = feedingTopic(prefix);
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["icon"] = "mdi:fish";
  doc["uniq_id"] = "catfish_feeding";
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

String discoveryLastFeed(const String &prefix) {
  JsonDocument doc;
  doc["name"] = "Last feed";
  doc["state_topic"] = lastFeedTopic(prefix);
  doc["device_class"] = "timestamp";
  doc["uniq_id"] = "catfish_last_feed";
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

String discoveryNextFeed(const String &prefix) {
  JsonDocument doc;
  doc["name"] = "Next feed";
  doc["state_topic"] = nextFeedTopic(prefix);
  doc["icon"] = "mdi:clock-outline";
  doc["uniq_id"] = "catfish_next_feed";
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

// ------------------------------------------------- v1.9.0 discovery configs
String discoveryPaused(const String &prefix) {
  JsonDocument doc;
  doc["name"] = "Feeding paused";
  doc["state_topic"] = pausedTopic(prefix);
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["icon"] = "mdi:fish-off";
  doc["uniq_id"] = "catfish_paused";
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

String discoveryFeedsToday(const String &prefix) {
  JsonDocument doc;
  doc["name"] = "Feeds today";
  doc["state_topic"] = feedsTodayTopic(prefix);
  doc["unit_of_measurement"] = "feeds";
  doc["icon"] = "mdi:counter";
  doc["uniq_id"] = "catfish_feeds_today";
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

String discoveryFailsToday(const String &prefix) {
  JsonDocument doc;
  doc["name"] = "Feed failures today";
  doc["state_topic"] = failsTodayTopic(prefix);
  doc["unit_of_measurement"] = "fails";
  doc["icon"] = "mdi:alert-circle-outline";
  doc["uniq_id"] = "catfish_fails_today";
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

// ---------------------------------------------------------- per-tank (P5)
// Numeric pieces go through snprintf: the native-test mock String
// (std::string) has no operator+(int), and this keeps device + host identical.
String tankFeedCmdTopic(const String &prefix, uint8_t tank1) {
  String p = prefix.length() ? prefix : String(CATFISH_MQTT_PREFIX);
  char leaf[24];
  snprintf(leaf, sizeof(leaf), "/tank%u/feed/cmd", (unsigned)tank1);
  return p + leaf;
}

String tankOnlineTopic(const String &prefix, uint8_t tank1) {
  String p = prefix.length() ? prefix : String(CATFISH_MQTT_PREFIX);
  char leaf[20];
  snprintf(leaf, sizeof(leaf), "/tank%u/online", (unsigned)tank1);
  return p + leaf;
}

String tankDiscoveryPath(const char *component, uint8_t tank1) {
  char path[64];
  snprintf(path, sizeof(path), "homeassistant/%s/catfish_tank%u/config",
           component, (unsigned)tank1);
  return String(path);
}

String tankDiscoveryPathObj(const char *component, uint8_t tank1,
                            const char *obj) {
  char path[80];
  snprintf(path, sizeof(path), "homeassistant/%s/catfish_tank%u_%s/config",
           component, (unsigned)tank1, obj);
  return String(path);
}

String discoveryTankFeedButton(const String &prefix, uint8_t tank1,
                               const String &tankName) {
  JsonDocument doc;
  char name[40];
  if (tankName.length())
    snprintf(name, sizeof(name), "Feed %s", tankName.c_str());
  else
    snprintf(name, sizeof(name), "Feed tank %u", (unsigned)tank1);
  doc["name"] = name;
  doc["command_topic"] = tankFeedCmdTopic(prefix, tank1);
  doc["payload_press"] = "ON";
  doc["icon"] = "mdi:fish-food";
  char uniq[24];
  snprintf(uniq, sizeof(uniq), "catfish_tank%u_feed", (unsigned)tank1);
  doc["uniq_id"] = uniq;
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

String discoveryTankOnline(const String &prefix, uint8_t tank1,
                           const String &tankName) {
  JsonDocument doc;
  char name[36];
  if (tankName.length())
    snprintf(name, sizeof(name), "%s online", tankName.c_str());
  else
    snprintf(name, sizeof(name), "Tank %u online", (unsigned)tank1);
  doc["name"] = name;
  doc["state_topic"] = tankOnlineTopic(prefix, tank1);
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["device_class"] = "connectivity";
  char uniq[26];
  snprintf(uniq, sizeof(uniq), "catfish_tank%u_online", (unsigned)tank1);
  doc["uniq_id"] = uniq;
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

// ------------------------------------------- v1.9.0 per-tank sensor entities
String discoveryTankLastFeed(const String &prefix, uint8_t tank1,
                             const String &tankName) {
  JsonDocument doc;
  char name[40];
  if (tankName.length())
    snprintf(name, sizeof(name), "%s last feed", tankName.c_str());
  else
    snprintf(name, sizeof(name), "Tank %u last feed", (unsigned)tank1);
  doc["name"] = name;
  doc["state_topic"] = tankLastFeedTopic(prefix, tank1);
  doc["device_class"] = "timestamp";
  char uniq[28];
  snprintf(uniq, sizeof(uniq), "catfish_tank%u_last_feed", (unsigned)tank1);
  doc["uniq_id"] = uniq;
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

String discoveryTankSeconds(const String &prefix, uint8_t tank1,
                            const String &tankName) {
  JsonDocument doc;
  char name[40];
  if (tankName.length())
    snprintf(name, sizeof(name), "%s feed seconds", tankName.c_str());
  else
    snprintf(name, sizeof(name), "Tank %u feed seconds", (unsigned)tank1);
  doc["name"] = name;
  doc["state_topic"] = tankSecondsTopic(prefix, tank1);
  doc["unit_of_measurement"] = "s";
  doc["icon"] = "mdi:timer";
  char uniq[28];
  snprintf(uniq, sizeof(uniq), "catfish_tank%u_seconds", (unsigned)tank1);
  doc["uniq_id"] = uniq;
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

String discoveryTankFood(const String &prefix, uint8_t tank1,
                         const String &tankName) {
  JsonDocument doc;
  char name[36];
  if (tankName.length())
    snprintf(name, sizeof(name), "%s food level", tankName.c_str());
  else
    snprintf(name, sizeof(name), "Tank %u food level", (unsigned)tank1);
  doc["name"] = name;
  doc["state_topic"] = tankFoodTopic(prefix, tank1);
  doc["unit_of_measurement"] = "%";
  doc["icon"] = "mdi:silo-outline";
  char uniq[26];
  snprintf(uniq, sizeof(uniq), "catfish_tank%u_food", (unsigned)tank1);
  doc["uniq_id"] = uniq;
  addAvailability(doc, prefix);
  addDevice(doc);
  return serialize(doc);
}

// ---------------------------------------------------------------- payloads
String epochToIso8601(uint32_t epoch) {
  if (epoch == 0) return String("unknown");
  const time_t t = (time_t)epoch;
  struct tm tmUtc;
  if (!gmtime_r(&t, &tmUtc)) return String("unknown");
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
           tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);
  return String(buf);
}

uint8_t clampPulse(long seconds) {
  if (seconds < 1) return 1;
  if (seconds > (long)CATFISH_PULSE_S_LIMIT) return CATFISH_PULSE_S_LIMIT;
  return (uint8_t)seconds;
}

bool parseFeedCmd(const String &payload, uint8_t *secs, uint8_t *mask) {
  if (secs) *secs = 0; // 0 = device default pulse
  if (mask) *mask = 0; // 0 = all configured tanks
  if (payload == "ON") return true; // legacy aggregate button
  const char *s = payload.c_str();
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  if (*s != '{') return false;
  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return false;
  if (!doc["seconds"].isNull()) {
    const long v = doc["seconds"] | -1L;
    if (v < 1 || v > (long)CATFISH_PULSE_S_LIMIT) return false;
    if (secs) *secs = (uint8_t)v;
  }
  if (!doc["tanks"].isNull()) {
    const long t = doc["tanks"] | -1L;
    if (t < 1 || t > 15) return false;
    if (mask) *mask = (uint8_t)t;
  }
  return true;
}

} // namespace mqtt_payloads
