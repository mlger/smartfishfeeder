/**
 * @file test_mqtt_payloads.cpp
 * @brief Host-side unit tests for the pure MQTT/HA payload builders.
 *
 * Runs with:  pio test -d firmware/display-unit -e native
 *
 * Compiles the real mqtt_payloads.cpp via #include (the native env excludes
 * src/ with src_filter) against a mock Arduino.h (String = std::string).
 */
#include <unity.h>

#include <string>

#include "../../src/mqtt_payloads.cpp" // NOLINT (deliberate single-TU build)

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// topics
// ---------------------------------------------------------------------------
void test_make_topic_falls_back_to_default_prefix(void) {
  TEST_ASSERT_EQUAL_STRING("catfish/feeding",
                           mqtt_payloads::makeTopic("", "feeding").c_str());
}

void test_all_topics_under_custom_prefix(void) {
  const String p = "tank1";
  TEST_ASSERT_EQUAL_STRING("tank1/availability",
                           mqtt_payloads::availabilityTopic(p).c_str());
  TEST_ASSERT_EQUAL_STRING("tank1/feed/cmd",
                           mqtt_payloads::feedCmdTopic(p).c_str());
  TEST_ASSERT_EQUAL_STRING("tank1/pulse/set",
                           mqtt_payloads::pulseSetTopic(p).c_str());
  TEST_ASSERT_EQUAL_STRING("tank1/pulse/state",
                           mqtt_payloads::pulseStateTopic(p).c_str());
  TEST_ASSERT_EQUAL_STRING("tank1/feeding",
                           mqtt_payloads::feedingTopic(p).c_str());
  TEST_ASSERT_EQUAL_STRING("tank1/relay/online",
                           mqtt_payloads::relayOnlineTopic(p).c_str());
  TEST_ASSERT_EQUAL_STRING("tank1/last_feed",
                           mqtt_payloads::lastFeedTopic(p).c_str());
  TEST_ASSERT_EQUAL_STRING("tank1/next_feed",
                           mqtt_payloads::nextFeedTopic(p).c_str());
}

// ---------------------------------------------------------------------------
// discovery configs
// ---------------------------------------------------------------------------
void test_discovery_button_config(void) {
  const String j = mqtt_payloads::discoveryFeedButton("catfish");
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"name\":\"Feed now\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"command_topic\":\"catfish/feed/cmd\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"payload_press\":\"ON\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"uniq_id\":\"catfish_feed_now\""));
  TEST_ASSERT_NOT_NULL(
      strstr(j.c_str(), "\"availability_topic\":\"catfish/availability\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"payload_available\":\"online\""));
  TEST_ASSERT_NOT_NULL(
      strstr(j.c_str(), "\"payload_not_available\":\"offline\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"identifiers\":[\"catfish-display\"]"));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"sw_version\":\"" CATFISH_APP_VERSION "\""));
}

void test_discovery_pulse_number_config(void) {
  const String j = mqtt_payloads::discoveryPulseNumber("catfish");
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"state_topic\":\"catfish/pulse/state\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"command_topic\":\"catfish/pulse/set\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"min\":1"));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"max\":30"));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"unit_of_measurement\":\"s\""));
}

void test_discovery_relay_online_config(void) {
  const String j = mqtt_payloads::discoveryRelayOnline("catfish");
  TEST_ASSERT_NOT_NULL(
      strstr(j.c_str(), "\"state_topic\":\"catfish/relay/online\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"device_class\":\"connectivity\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"payload_on\":\"ON\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"payload_off\":\"OFF\""));
}

void test_discovery_feeding_config(void) {
  const String j = mqtt_payloads::discoveryFeeding("catfish");
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"state_topic\":\"catfish/feeding\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"payload_on\":\"ON\""));
}

void test_discovery_last_feed_is_timestamp_sensor(void) {
  const String j = mqtt_payloads::discoveryLastFeed("catfish");
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"state_topic\":\"catfish/last_feed\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"device_class\":\"timestamp\""));
}

void test_discovery_next_feed_config(void) {
  const String j = mqtt_payloads::discoveryNextFeed("catfish");
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"state_topic\":\"catfish/next_feed\""));
}

// ---------------------------------------------------------------------------
// per-tank entities (Phase 5)
// ---------------------------------------------------------------------------
void test_tank_topics(void) {
  TEST_ASSERT_EQUAL_STRING("catfish/tank2/feed/cmd",
                           mqtt_payloads::tankFeedCmdTopic("catfish", 2).c_str());
  TEST_ASSERT_EQUAL_STRING("catfish/tank3/online",
                           mqtt_payloads::tankOnlineTopic("catfish", 3).c_str());
  // empty prefix falls back to the default like the shared builders
  TEST_ASSERT_EQUAL_STRING("catfish/tank1/feed/cmd",
                           mqtt_payloads::tankFeedCmdTopic("", 1).c_str());
}

void test_tank_discovery_paths(void) {
  TEST_ASSERT_EQUAL_STRING("homeassistant/button/catfish_tank1/config",
                           mqtt_payloads::tankDiscoveryPath("button", 1).c_str());
  TEST_ASSERT_EQUAL_STRING(
      "homeassistant/binary_sensor/catfish_tank4/config",
      mqtt_payloads::tankDiscoveryPath("binary_sensor", 4).c_str());
}

void test_tank_feed_button_config(void) {
  const String j = mqtt_payloads::discoveryTankFeedButton("catfish", 2, "Pond");
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"name\":\"Feed Pond\""));
  TEST_ASSERT_NOT_NULL(
      strstr(j.c_str(), "\"command_topic\":\"catfish/tank2/feed/cmd\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"payload_press\":\"ON\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"uniq_id\":\"catfish_tank2_feed\""));
  TEST_ASSERT_NOT_NULL(
      strstr(j.c_str(), "\"availability_topic\":\"catfish/availability\""));
  // empty name falls back to a generic label
  const String j2 = mqtt_payloads::discoveryTankFeedButton("catfish", 1, "");
  TEST_ASSERT_NOT_NULL(strstr(j2.c_str(), "\"name\":\"Feed tank 1\""));
}

void test_tank_online_config(void) {
  const String j = mqtt_payloads::discoveryTankOnline("catfish", 1, "Main");
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"name\":\"Main online\""));
  TEST_ASSERT_NOT_NULL(
      strstr(j.c_str(), "\"state_topic\":\"catfish/tank1/online\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"device_class\":\"connectivity\""));
  TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"uniq_id\":\"catfish_tank1_online\""));
  const String j2 = mqtt_payloads::discoveryTankOnline("catfish", 3, "");
  TEST_ASSERT_NOT_NULL(strstr(j2.c_str(), "\"name\":\"Tank 3 online\""));
}

// ---------------------------------------------------------------------------
// payload helpers
// ---------------------------------------------------------------------------
void test_epoch_to_iso8601_utc(void) {
  // 2025-01-01T00:00:00Z == 1735689600; +4 d +7 h 30 m == 1736062200
  TEST_ASSERT_EQUAL_STRING("2025-01-05T07:30:00Z",
                           mqtt_payloads::epochToIso8601(1736062200).c_str());
}

void test_epoch_zero_means_never(void) {
  TEST_ASSERT_EQUAL_STRING("unknown", mqtt_payloads::epochToIso8601(0).c_str());
}

void test_clamp_pulse_matches_firmware_limits(void) {
  TEST_ASSERT_EQUAL_UINT8(1, mqtt_payloads::clampPulse(0));
  TEST_ASSERT_EQUAL_UINT8(1, mqtt_payloads::clampPulse(-5));
  TEST_ASSERT_EQUAL_UINT8(7, mqtt_payloads::clampPulse(7));
  TEST_ASSERT_EQUAL_UINT8(CATFISH_PULSE_S_LIMIT, mqtt_payloads::clampPulse(999));
  TEST_ASSERT_EQUAL_UINT8(30, mqtt_payloads::clampPulse(999)); // sanity: limit is 30
}

// ---------------------------------------------------------------------------
// v1.9.0: state topics + per-tank object discovery paths
// ---------------------------------------------------------------------------
void test_v190_state_topics(void) {
  const String p = "catfish";
  TEST_ASSERT_EQUAL_STRING("catfish/paused", mqtt_payloads::pausedTopic(p).c_str());
  TEST_ASSERT_EQUAL_STRING("catfish/feeds_today",
                           mqtt_payloads::feedsTodayTopic(p).c_str());
  TEST_ASSERT_EQUAL_STRING("catfish/fails_today",
                           mqtt_payloads::failsTodayTopic(p).c_str());
  // tank1 spot-checks
  TEST_ASSERT_EQUAL_STRING("catfish/tank1/last_feed",
                           mqtt_payloads::tankLastFeedTopic(p, 1).c_str());
  TEST_ASSERT_EQUAL_STRING("catfish/tank1/seconds",
                           mqtt_payloads::tankSecondsTopic(p, 1).c_str());
  TEST_ASSERT_EQUAL_STRING("catfish/tank1/food",
                           mqtt_payloads::tankFoodTopic(p, 1).c_str());
  // empty prefix falls back to the default root like every other builder
  TEST_ASSERT_EQUAL_STRING("catfish/paused", mqtt_payloads::pausedTopic("").c_str());
  TEST_ASSERT_EQUAL_STRING("catfish/tank2/food",
                           mqtt_payloads::tankFoodTopic("", 2).c_str());
}

void test_v190_tank_discovery_path_obj(void) {
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/catfish_tank1_last_feed/config",
                           mqtt_payloads::tankDiscoveryPathObj("sensor", 1,
                                                               "last_feed")
                               .c_str());
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/catfish_tank2_food/config",
                           mqtt_payloads::tankDiscoveryPathObj("sensor", 2,
                                                               "food")
                               .c_str());
}

void test_v190_discovery_payloads_parse_as_json(void) {
  const String paused = mqtt_payloads::discoveryPaused("catfish");
  const String today = mqtt_payloads::discoveryFeedsToday("catfish");
  const String fails = mqtt_payloads::discoveryFailsToday("catfish");
  const String tlf = mqtt_payloads::discoveryTankLastFeed("catfish", 1, "Pond");
  const String tsec = mqtt_payloads::discoveryTankSeconds("catfish", 1, "");
  const String tfood = mqtt_payloads::discoveryTankFood("catfish", 1, "Pond");

  JsonDocument d;
  TEST_ASSERT_TRUE(deserializeJson(d, paused) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("catfish/paused", d["state_topic"] | "");
  TEST_ASSERT_EQUAL_STRING("catfish_paused", d["uniq_id"] | "");
  TEST_ASSERT_EQUAL_STRING("ON", d["payload_on"] | "");
  TEST_ASSERT_EQUAL_STRING("offline", d["payload_not_available"] | "");

  TEST_ASSERT_TRUE(deserializeJson(d, today) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("catfish/feeds_today", d["state_topic"] | "");
  TEST_ASSERT_EQUAL_STRING("feeds", d["unit_of_measurement"] | "");

  TEST_ASSERT_TRUE(deserializeJson(d, fails) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("catfish/fails_today", d["state_topic"] | "");
  TEST_ASSERT_EQUAL_STRING("fails", d["unit_of_measurement"] | "");

  TEST_ASSERT_TRUE(deserializeJson(d, tlf) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("Pond last feed", d["name"] | "");
  TEST_ASSERT_EQUAL_STRING("catfish/tank1/last_feed", d["state_topic"] | "");
  TEST_ASSERT_EQUAL_STRING("timestamp", d["device_class"] | "");

  TEST_ASSERT_TRUE(deserializeJson(d, tsec) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("Tank 1 feed seconds", d["name"] | ""); // empty name fallback
  TEST_ASSERT_EQUAL_STRING("catfish/tank1/seconds", d["state_topic"] | "");
  TEST_ASSERT_EQUAL_STRING("s", d["unit_of_measurement"] | "");

  TEST_ASSERT_TRUE(deserializeJson(d, tfood) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("catfish/tank1/food", d["state_topic"] | "");
  TEST_ASSERT_EQUAL_STRING("%", d["unit_of_measurement"] | "");
  TEST_ASSERT_EQUAL_STRING("catfish_tank1_food", d["uniq_id"] | "");
  TEST_ASSERT_NOT_NULL(strstr(tfood.c_str(), "\"identifiers\":[\"catfish-display\"]"));
}

// ---------------------------------------------------------------------------
// v1.9.0 (F1): parseFeedCmd
// ---------------------------------------------------------------------------
void test_parse_feed_cmd_legacy_on(void) {
  uint8_t secs = 99, mask = 99;
  TEST_ASSERT_TRUE(mqtt_payloads::parseFeedCmd("ON", &secs, &mask));
  TEST_ASSERT_EQUAL_UINT8(0, secs); // 0 = device default pulse
  TEST_ASSERT_EQUAL_UINT8(0, mask); // 0 = all configured tanks
}

void test_parse_feed_cmd_json_fields(void) {
  uint8_t secs = 0, mask = 0;
  TEST_ASSERT_TRUE(mqtt_payloads::parseFeedCmd("{\"seconds\":7}", &secs, &mask));
  TEST_ASSERT_EQUAL_UINT8(7, secs);
  TEST_ASSERT_EQUAL_UINT8(0, mask);

  TEST_ASSERT_TRUE(
      mqtt_payloads::parseFeedCmd("{\"seconds\":7,\"tanks\":5}", &secs, &mask));
  TEST_ASSERT_EQUAL_UINT8(7, secs);
  TEST_ASSERT_EQUAL_UINT8(5, mask); // raw mask 0b101 = tanks 1+3

  TEST_ASSERT_TRUE(mqtt_payloads::parseFeedCmd("{\"tanks\":15}", &secs, &mask));
  TEST_ASSERT_EQUAL_UINT8(0, secs);
  TEST_ASSERT_EQUAL_UINT8(15, mask); // max accepted mask
}

void test_parse_feed_cmd_rejects_invalid_payloads(void) {
  uint8_t secs = 7, mask = 7;
  TEST_ASSERT_FALSE(mqtt_payloads::parseFeedCmd("{\"seconds\":0}", &secs, &mask));
  TEST_ASSERT_FALSE(mqtt_payloads::parseFeedCmd("{\"seconds\":99}", &secs, &mask));
  TEST_ASSERT_FALSE(mqtt_payloads::parseFeedCmd("{\"tanks\":16}", &secs, &mask));
  TEST_ASSERT_FALSE(mqtt_payloads::parseFeedCmd("garbage", &secs, &mask));
  TEST_ASSERT_FALSE(mqtt_payloads::parseFeedCmd("", &secs, &mask));
  // null pointers must not crash either
  TEST_ASSERT_TRUE(mqtt_payloads::parseFeedCmd("ON", nullptr, nullptr));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_make_topic_falls_back_to_default_prefix);
  RUN_TEST(test_all_topics_under_custom_prefix);
  RUN_TEST(test_discovery_button_config);
  RUN_TEST(test_discovery_pulse_number_config);
  RUN_TEST(test_discovery_relay_online_config);
  RUN_TEST(test_discovery_feeding_config);
  RUN_TEST(test_discovery_last_feed_is_timestamp_sensor);
  RUN_TEST(test_discovery_next_feed_config);
  RUN_TEST(test_tank_topics);
  RUN_TEST(test_tank_discovery_paths);
  RUN_TEST(test_tank_feed_button_config);
  RUN_TEST(test_tank_online_config);
  RUN_TEST(test_epoch_to_iso8601_utc);
  RUN_TEST(test_epoch_zero_means_never);
  RUN_TEST(test_clamp_pulse_matches_firmware_limits);
  RUN_TEST(test_v190_state_topics);
  RUN_TEST(test_v190_tank_discovery_path_obj);
  RUN_TEST(test_v190_discovery_payloads_parse_as_json);
  RUN_TEST(test_parse_feed_cmd_legacy_on);
  RUN_TEST(test_parse_feed_cmd_json_fields);
  RUN_TEST(test_parse_feed_cmd_rejects_invalid_payloads);
  return UNITY_END();
}
