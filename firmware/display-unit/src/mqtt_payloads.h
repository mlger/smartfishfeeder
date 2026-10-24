/**
 * @file mqtt_payloads.h
 * @brief Pure MQTT topic/payload builders for Home Assistant auto-discovery.
 *
 * Dependency-free apart from ArduinoJson + Arduino String, so the whole module
 * compiles in the [env:native] host-test environment (mock Arduino.h).
 * No I/O, no state: every function maps inputs -> String output.
 *
 * Topic layout (prefix defaults to "catfish"):
 *   <prefix>/availability      "online"/"offline" (retained, LWT offline)
 *   <prefix>/last_feed         ISO-8601 UTC timestamp or "unknown"
 *   <prefix>/next_feed         "HH:MM" or "none"
 *   <prefix>/feeding           "ON"/"OFF"
 *   <prefix>/relay/online      "ON"/"OFF"
 *   <prefix>/pulse/state       seconds (decimal string)
 *   <prefix>/feed/cmd          subscribe: "ON"  -> feed with default pulse
 *   <prefix>/pulse/set         subscribe: "1".."30" -> default pulse
 *
 * Discovery configs are published (retained) to the HA-standard
 *   homeassistant/<component>/catfish_<object>/config
 */
#ifndef CATFISH_MQTT_PAYLOADS_H
#define CATFISH_MQTT_PAYLOADS_H

#include <Arduino.h>
#include <stdint.h>

namespace mqtt_payloads {

// ---- state/command topics -------------------------------------------------
String makeTopic(const String &prefix, const char *leaf);
String availabilityTopic(const String &prefix);
String feedCmdTopic(const String &prefix);
String pulseSetTopic(const String &prefix);
String pulseStateTopic(const String &prefix);
String feedingTopic(const String &prefix);
String relayOnlineTopic(const String &prefix);
String lastFeedTopic(const String &prefix);
String nextFeedTopic(const String &prefix);

// ---- discovery config payloads (retained) ---------------------------------
String discoveryFeedButton(const String &prefix);     // button.feed_now
String discoveryPulseNumber(const String &prefix);    // number.pulse_seconds
String discoveryRelayOnline(const String &prefix);    // binary_sensor.connectivity
String discoveryFeeding(const String &prefix);        // binary_sensor.feeding
String discoveryLastFeed(const String &prefix);       // sensor.timestamp
String discoveryNextFeed(const String &prefix);       // sensor (plain string)

// ---- payload helpers -------------------------------------------------------
/** Unix epoch -> "YYYY-MM-DDTHH:MM:SSZ" (HA device_class timestamp); 0 -> "unknown". */
String epochToIso8601(uint32_t epoch);
/** Clamp a pulse value from an MQTT command into 1..CATFISH_PULSE_S_LIMIT. */
uint8_t clampPulse(long seconds);

} // namespace mqtt_payloads

#endif // CATFISH_MQTT_PAYLOADS_H
