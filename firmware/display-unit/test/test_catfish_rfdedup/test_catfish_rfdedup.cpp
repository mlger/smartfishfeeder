/**
 * @file test_catfish_rfdedup.cpp
 * @brief Host-side unit tests for the relay direct-feed dedup machine (v1.13.0).
 *
 * Pins the coordination contract between the display hub (seq echo in /feed)
 * and the relay's direct ESP-NOW listener — see catfish_rfdedup.h.
 */
#include <unity.h>

#include <catfish_rfdedup.h>
#include <string.h>

static CatfishRfDedup d;

void setUp(void) { catfishRfDedupInit(&d); }
void tearDown(void) {}

void test_init_clears_everything(void) {
  TEST_ASSERT_FALSE(d.hasPending);
  for (size_t i = 0; i < CATFISH_RFDEDUP_RING; i++) {
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, d.ring[i]);
  }
}

void test_arm_then_poll_before_grace_is_none(void) {
  TEST_ASSERT_TRUE(catfishRfDedupArm(&d, 42, 5, 1000));
  TEST_ASSERT_TRUE(d.hasPending);
  uint16_t seq = 0;
  uint8_t secs = 0;
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFDEDUP_NONE,
                          catfishRfDedupPoll(&d, 1000 + CATFISH_RFDEDUP_GRACE_MS - 1, &seq, &secs));
  TEST_ASSERT_TRUE(d.hasPending); // still armed
}

void test_poll_after_grace_returns_feed_and_adds_ring(void) {
  TEST_ASSERT_TRUE(catfishRfDedupArm(&d, 42, 5, 1000));
  uint16_t seq = 0;
  uint8_t secs = 0;
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFDEDUP_FEED,
                          catfishRfDedupPoll(&d, 1000 + CATFISH_RFDEDUP_GRACE_MS, &seq, &secs));
  TEST_ASSERT_EQUAL_UINT16(42, seq);
  TEST_ASSERT_EQUAL_UINT8(5, secs);
  TEST_ASSERT_FALSE(d.hasPending); // slot consumed
  TEST_ASSERT_TRUE(catfishRfDedupRingHas(&d, 42)); // marked executed
}

void test_cancel_pending_beats_grace(void) {
  TEST_ASSERT_TRUE(catfishRfDedupArm(&d, 7, 0, 1000));
  catfishRfDedupCancelPending(&d); // the hub's /feed arrived in time
  uint16_t seq = 0;
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFDEDUP_NONE,
                          catfishRfDedupPoll(&d, 1000 + CATFISH_RFDEDUP_GRACE_MS + 5000, &seq, nullptr));
  TEST_ASSERT_FALSE(catfishRfDedupRingHas(&d, 7)); // cancelled, NOT executed
}

void test_hub_echo_seq_lands_in_ring_and_blocks_direct(void) {
  // hub POSTed /feed with "seq":42 -> ring add; the datagram then arrives
  catfishRfDedupRingAdd(&d, 42);
  TEST_ASSERT_TRUE(catfishRfDedupRingHas(&d, 42));
  TEST_ASSERT_FALSE(catfishRfDedupArm(&d, 42, 5, 1000)); // recv path must drop
  TEST_ASSERT_FALSE(d.hasPending);
}

void test_rearm_same_seq_refreshes_grace(void) {
  TEST_ASSERT_TRUE(catfishRfDedupArm(&d, 9, 3, 1000));
  TEST_ASSERT_TRUE(catfishRfDedupArm(&d, 9, 3, 2000)); // puck re-broadcast
  uint16_t seq = 0;
  // would have expired vs the first arm, but the re-arm pushed the deadline
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFDEDUP_NONE,
                          catfishRfDedupPoll(&d, 1000 + CATFISH_RFDEDUP_GRACE_MS + 100, &seq, nullptr));
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFDEDUP_FEED,
                          catfishRfDedupPoll(&d, 2000 + CATFISH_RFDEDUP_GRACE_MS, &seq, nullptr));
  TEST_ASSERT_EQUAL_UINT16(9, seq);
}

void test_new_seq_replaces_pending(void) {
  TEST_ASSERT_TRUE(catfishRfDedupArm(&d, 1, 2, 1000));
  TEST_ASSERT_TRUE(catfishRfDedupArm(&d, 2, 8, 1100)); // single feeder, last wins
  uint16_t seq = 0;
  uint8_t secs = 0;
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFDEDUP_FEED,
                          catfishRfDedupPoll(&d, 1100 + CATFISH_RFDEDUP_GRACE_MS, &seq, &secs));
  TEST_ASSERT_EQUAL_UINT16(2, seq);
  TEST_ASSERT_EQUAL_UINT8(8, secs);
}

void test_ring_wraps_after_capacity(void) {
  for (uint16_t s = 1; s <= (uint16_t)CATFISH_RFDEDUP_RING; s++) {
    catfishRfDedupRingAdd(&d, s);
  }
  // ring is exactly full: every entry still present
  TEST_ASSERT_TRUE(catfishRfDedupRingHas(&d, 1));
  TEST_ASSERT_TRUE(catfishRfDedupRingHas(&d, CATFISH_RFDEDUP_RING));
  // 9th add evicts the oldest (seq 1)
  catfishRfDedupRingAdd(&d, 99);
  TEST_ASSERT_TRUE(catfishRfDedupRingHas(&d, 99));
  TEST_ASSERT_FALSE(catfishRfDedupRingHas(&d, 1));
  TEST_ASSERT_TRUE(catfishRfDedupRingHas(&d, 2)); // not yet evicted
  // 10th add evicts seq 2
  catfishRfDedupRingAdd(&d, 100);
  TEST_ASSERT_TRUE(catfishRfDedupRingHas(&d, 100));
  TEST_ASSERT_FALSE(catfishRfDedupRingHas(&d, 2));
  TEST_ASSERT_TRUE(catfishRfDedupRingHas(&d, 99)); // recent stays
}

void test_seq_zero_is_never_armed_or_rung(void) {
  TEST_ASSERT_FALSE(catfishRfDedupArm(&d, 0, 5, 1000)); // 0 = hub-only feed
  catfishRfDedupRingAdd(&d, 0); // no-op
  // explicit contract: ringHas(0) reports "seen" so recv paths drop it early
  TEST_ASSERT_TRUE(catfishRfDedupRingHas(&d, 0));
  TEST_ASSERT_FALSE(d.hasPending);
}

void test_millis_wraparound_still_expires(void) {
  TEST_ASSERT_TRUE(catfishRfDedupArm(&d, 5, 4, 0xFFFFF000u)); // near the top
  uint16_t seq = 0;
  // 0xFFFFF000 + 1.5 s wraps past 2^32 -> small unsigned value
  const uint32_t afterWrap = 0xFFFFF000u + CATFISH_RFDEDUP_GRACE_MS;
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFDEDUP_FEED,
                          catfishRfDedupPoll(&d, afterWrap, &seq, nullptr));
  TEST_ASSERT_EQUAL_UINT16(5, seq);
}

void test_poll_with_no_pending_is_none(void) {
  uint16_t seq = 0x5A5A;
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFDEDUP_NONE,
                          catfishRfDedupPoll(&d, 123456, &seq, nullptr));
  TEST_ASSERT_EQUAL_UINT16(0x5A5A, seq); // untouched on NONE
}

void test_grace_boundary_is_inclusive(void) {
  TEST_ASSERT_TRUE(catfishRfDedupArm(&d, 3, 0, 0));
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFDEDUP_NONE,
                          catfishRfDedupPoll(&d, CATFISH_RFDEDUP_GRACE_MS - 1, nullptr, nullptr));
  TEST_ASSERT_EQUAL_UINT8(CATFISH_RFDEDUP_FEED,
                          catfishRfDedupPoll(&d, CATFISH_RFDEDUP_GRACE_MS, nullptr, nullptr));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_init_clears_everything);
  RUN_TEST(test_arm_then_poll_before_grace_is_none);
  RUN_TEST(test_poll_after_grace_returns_feed_and_adds_ring);
  RUN_TEST(test_cancel_pending_beats_grace);
  RUN_TEST(test_hub_echo_seq_lands_in_ring_and_blocks_direct);
  RUN_TEST(test_rearm_same_seq_refreshes_grace);
  RUN_TEST(test_new_seq_replaces_pending);
  RUN_TEST(test_ring_wraps_after_capacity);
  RUN_TEST(test_seq_zero_is_never_armed_or_rung);
  RUN_TEST(test_millis_wraparound_still_expires);
  RUN_TEST(test_poll_with_no_pending_is_none);
  RUN_TEST(test_grace_boundary_is_inclusive);
  return UNITY_END();
}
