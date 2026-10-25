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

// ---- v1.9.0 state topics ---------------------------------------------------
String pausedTopic(const String &prefix);        // "ON"/"OFF" (A5 vacation)
String feedsTodayTopic(const String &prefix);    // count (C3)
String failsTodayTopic(const String &prefix);    // count (C3)
String tankLastFeedTopic(const String &prefix, uint8_t tank1); // ISO-8601 (F2)
String tankSecondsTopic(const String &prefix, uint8_t tank1);  // seconds (F2)
String tankFoodTopic(const String &prefix, uint8_t tank1);     // percent (D1)

// ---- discovery config payloads (retained) ---------------------------------
String discoveryFeedButton(const String &prefix);     // button.feed_now
String discoveryPulseNumber(const String &prefix);    // number.pulse_seconds
String discoveryRelayOnline(const String &prefix);    // binary_sensor.connectivity
String discoveryFeeding(const String &prefix);        // binary_sensor.feeding
String discoveryLastFeed(const String &prefix);       // sensor.timestamp
String discoveryNextFeed(const String &prefix);       // sensor (plain string)

// ---- v1.9.0 discovery configs ---------------------------------------------
String discoveryPaused(const String &prefix);         // binary_sensor (A5)
String discoveryFeedsToday(const String &prefix);     // sensor (C3)
String discoveryFailsToday(const String &prefix);     // sensor (C3)
String discoveryTankLastFeed(const String &prefix, uint8_t tank1,
                             const String &tankName); // sensor.timestamp (F2)
String discoveryTankSeconds(const String &prefix, uint8_t tank1,
                            const String &tankName);  // sensor "s" (F2)
String discoveryTankFood(const String &prefix, uint8_t tank1,
                         const String &tankName);     // sensor "%" (D1)

// ---- per-tank entities (Phase 5) -------------------------------------------
String tankFeedCmdTopic(const String &prefix, uint8_t tank1);
String tankOnlineTopic(const String &prefix, uint8_t tank1);
/** HA discovery config for a per-tank feed button ("Feed <tankName>"). */
String discoveryTankFeedButton(const String &prefix, uint8_t tank1,
                               const String &tankName);
/** HA discovery config for a per-tank online binary_sensor. */
String discoveryTankOnline(const String &prefix, uint8_t tank1,
                           const String &tankName);
/**
 * HA discovery config topic for a tank entity — publishing an EMPTY retained
 * payload to this path removes the entity from Home Assistant again.
 */
String tankDiscoveryPath(const char *component, uint8_t tank1);
/**
 * v1.9.0: discovery path with a per-tank OBJECT suffix — a tank can expose
 * several sensor entities (online / last_feed / seconds / food) and each needs
 * its own config topic.
 */
String tankDiscoveryPathObj(const char *component, uint8_t tank1,
                            const char *obj);

// ---- payload helpers -------------------------------------------------------
/** Unix epoch -> "YYYY-MM-DDTHH:MM:SSZ" (HA device_class timestamp); 0 -> "unknown". */
String epochToIso8601(uint32_t epoch);
/** Clamp a pulse value from an MQTT command into 1..CATFISH_PULSE_S_LIMIT. */
uint8_t clampPulse(long seconds);

/**
 * v1.9.0 (F1): parse a feed/cmd payload. Accepts the legacy "ON" (default
 * pulse, all tanks) and a JSON object {"seconds":1..30,"tanks":1..15}.
 * @param secs  out: clamped pulse seconds (0 = use the device default)
 * @param mask  out: target-tank bitmask (0 = all configured tanks)
 * @return false for anything else (ignored by the manager).
 */
bool parseFeedCmd(const String &payload, uint8_t *secs, uint8_t *mask);

} // namespace mqtt_payloads

#endif // CATFISH_MQTT_PAYLOADS_H
