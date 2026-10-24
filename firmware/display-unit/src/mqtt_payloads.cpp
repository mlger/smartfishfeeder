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

} // namespace mqtt_payloads
