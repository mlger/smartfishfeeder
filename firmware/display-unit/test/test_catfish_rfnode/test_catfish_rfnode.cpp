/**
 * @file test_catfish_rfnode.cpp
 * @brief Host-side unit tests for the RF-node datagram contract (v1.8.0).
 *
 * Pins the exact 12-byte wire format shared by the ESP-NOW and nRF24L01
 * transports — whatever these tests enforce is what the firmware parses.
 */
#include <unity.h>

#include <catfish_rfnode.h>
#include <string.h>

void test_build_produces_expected_bytes(void) {
  uint8_t buf[CATFISH_RFNODE_PKT_LEN];
  const uint32_t key = catfishRfnodeKeyFromToken("catfish");
  TEST_ASSERT_EQUAL_UINT(CATFISH_RFNODE_PKT_LEN,
                         catfishRfnodeBuild(buf, sizeof(buf), CATFISH_RFNODE_CMD_FEED,
                                            0x03, 5, 0x1234, key));
  TEST_ASSERT_EQUAL_UINT8(0x43, buf[0]);
  TEST_ASSERT_EQUAL_UINT8(0x46, buf[1]);
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFNODE_VER, buf[2]);
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFNODE_CMD_FEED, buf[3]);
  TEST_ASSERT_EQUAL_UINT8(0x03, buf[4]);
  TEST_ASSERT_EQUAL_UINT8(5, buf[5]);
  TEST_ASSERT_EQUAL_UINT8(0x34, buf[6]); // seq LE
  TEST_ASSERT_EQUAL_UINT8(0x12, buf[7]);
  TEST_ASSERT_EQUAL_UINT8(key & 0xFF, buf[8]);
  TEST_ASSERT_EQUAL_UINT8((key >> 8) & 0xFF, buf[9]);
  TEST_ASSERT_EQUAL_UINT8((key >> 16) & 0xFF, buf[10]);
  TEST_ASSERT_EQUAL_UINT8((key >> 24) & 0xFF, buf[11]);
}

void test_build_rejects_small_buffer(void) {
  uint8_t buf[CATFISH_RFNODE_PKT_LEN - 1];
  TEST_ASSERT_EQUAL_UINT(0, catfishRfnodeBuild(buf, sizeof(buf),
                                               CATFISH_RFNODE_CMD_FEED, 0, 0, 1,
                                               1));
  TEST_ASSERT_EQUAL_UINT(0, catfishRfnodeBuild(nullptr, 64,
                                               CATFISH_RFNODE_CMD_FEED, 0, 0, 1,
                                               1));
}

void test_parse_roundtrip(void) {
  uint8_t buf[CATFISH_RFNODE_PKT_LEN];
  const uint32_t key = catfishRfnodeKeyFromToken("tuna-2025");
  TEST_ASSERT_EQUAL_UINT(CATFISH_RFNODE_PKT_LEN,
                         catfishRfnodeBuild(buf, sizeof(buf), CATFISH_RFNODE_CMD_FEED,
                                            0x0F, 30, 0xABCD, key));

  uint8_t cmd = 0, mask = 0, seconds = 0;
  uint16_t seq = 0;
  uint32_t gotKey = 0;
  TEST_ASSERT_TRUE(catfishRfnodeParse(buf, sizeof(buf), &cmd, &mask, &seconds,
                                      &seq, &gotKey));
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFNODE_CMD_FEED, cmd);
  TEST_ASSERT_EQUAL_UINT8(0x0F, mask);
  TEST_ASSERT_EQUAL_UINT8(30, seconds);
  TEST_ASSERT_EQUAL_UINT16(0xABCD, seq);
  TEST_ASSERT_EQUAL_UINT32(key, gotKey);
}

void test_parse_rejects_truncated_and_garbage(void) {
  uint8_t buf[CATFISH_RFNODE_PKT_LEN];
  catfishRfnodeBuild(buf, sizeof(buf), CATFISH_RFNODE_CMD_FEED, 1, 3, 1, 42);

  uint8_t cmd = 0;
  TEST_ASSERT_FALSE(catfishRfnodeParse(buf, CATFISH_RFNODE_PKT_LEN - 1, &cmd,
                                       nullptr, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQUAL_UINT8(0xFF, cmd); // failure contract
  TEST_ASSERT_FALSE(catfishRfnodeParse(nullptr, 64, &cmd, nullptr, nullptr,
                                       nullptr, nullptr));

  buf[0] = 'X'; // bad magic
  TEST_ASSERT_FALSE(catfishRfnodeParse(buf, sizeof(buf), &cmd, nullptr,
                                       nullptr, nullptr, nullptr));
  buf[0] = 0x43;
  buf[1] = 'X';
  TEST_ASSERT_FALSE(catfishRfnodeParse(buf, sizeof(buf), &cmd, nullptr,
                                       nullptr, nullptr, nullptr));

  buf[1] = 0x46;
  buf[2] = 99; // unknown version
  TEST_ASSERT_FALSE(catfishRfnodeParse(buf, sizeof(buf), &cmd, nullptr,
                                       nullptr, nullptr, nullptr));

  buf[2] = CATFISH_RFNODE_VER;
  buf[3] = 7; // unknown command
  TEST_ASSERT_FALSE(catfishRfnodeParse(buf, sizeof(buf), &cmd, nullptr,
                                       nullptr, nullptr, nullptr));
}

void test_mask_clamp(void) {
  TEST_ASSERT_EQUAL_UINT8(0x00, catfishRfnodeClampMask(0x00));
  TEST_ASSERT_EQUAL_UINT8(0x0F, catfishRfnodeClampMask(0x0F));
  TEST_ASSERT_EQUAL_UINT8(0x05, catfishRfnodeClampMask(0xF5)); // high bits dropped
  TEST_ASSERT_EQUAL_UINT8(0x0F, catfishRfnodeClampMask(0xFF));
}

void test_seconds_clamp(void) {
  TEST_ASSERT_EQUAL_UINT8(0, catfishRfnodeClampSeconds(0)); // 0 = hub default
  TEST_ASSERT_EQUAL_UINT8(1, catfishRfnodeClampSeconds(1));
  TEST_ASSERT_EQUAL_UINT8(30, catfishRfnodeClampSeconds(30));
  TEST_ASSERT_EQUAL_UINT8(60, catfishRfnodeClampSeconds(60));
  TEST_ASSERT_EQUAL_UINT8(60, catfishRfnodeClampSeconds(200)); // hard cap
}

void test_key_derivation_is_deterministic_and_token_sensitive(void) {
  const uint32_t a1 = catfishRfnodeKeyFromToken("catfish");
  const uint32_t a2 = catfishRfnodeKeyFromToken("catfish");
  const uint32_t b = catfishRfnodeKeyFromToken("tuna-2025");
  TEST_ASSERT_EQUAL_UINT32(a1, a2);
  TEST_ASSERT_NOT_EQUAL(a1, b);
  // null/empty tokens must not crash; FNV-1a of "" is the offset basis, so
  // both map to the same value — distinct from any real token.
  const uint32_t n = catfishRfnodeKeyFromToken(nullptr);
  const uint32_t e = catfishRfnodeKeyFromToken("");
  TEST_ASSERT_EQUAL_UINT32(n, e);
  TEST_ASSERT_NOT_EQUAL(n, a1);
}

void test_build_parse_default_request(void) {
  // the minimal puck packet: all tanks default, hub default pulse
  uint8_t buf[CATFISH_RFNODE_PKT_LEN];
  const uint32_t key = catfishRfnodeKeyFromToken("catfish");
  catfishRfnodeBuild(buf, sizeof(buf), CATFISH_RFNODE_CMD_FEED, 0, 0, 77, key);
  uint8_t cmd = 0, mask = 0, seconds = 0;
  uint16_t seq = 0;
  uint32_t gotKey = 0;
  TEST_ASSERT_TRUE(catfishRfnodeParse(buf, sizeof(buf), &cmd, &mask, &seconds,
                                      &seq, &gotKey));
  TEST_ASSERT_EQUAL_UINT8(0, mask);   // hub resolves to the selected tank(s)
  TEST_ASSERT_EQUAL_UINT8(0, seconds); // hub resolves to the default pulse
  TEST_ASSERT_EQUAL_UINT32(key, gotKey);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_build_produces_expected_bytes);
  RUN_TEST(test_build_rejects_small_buffer);
  RUN_TEST(test_parse_roundtrip);
  RUN_TEST(test_parse_rejects_truncated_and_garbage);
  RUN_TEST(test_mask_clamp);
  RUN_TEST(test_seconds_clamp);
  RUN_TEST(test_key_derivation_is_deterministic_and_token_sensitive);
  RUN_TEST(test_build_parse_default_request);
  return UNITY_END();
}
