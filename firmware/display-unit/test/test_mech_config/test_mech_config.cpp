/**
 * @file test_mech_config.cpp
 * @brief Host-side unit tests for the shared feeding-mechanism helpers.
 *
 * Runs with:  pio test -d firmware/display-unit -e native
 *
 * catfish_mech.h is shared with the ESP8266 relay firmware: whatever these
 * tests pin down is exactly what the relay clamps /config values with.
 */
#include <unity.h>

#include <catfish_mech.h>

// ---------------------------------------------------------------- names
void test_mech_names_canonical(void) {
  TEST_ASSERT_EQUAL_STRING("motor", catfishMechName(CATFISH_MECH_MOTOR));
  TEST_ASSERT_EQUAL_STRING("servo", catfishMechName(CATFISH_MECH_SERVO));
  TEST_ASSERT_EQUAL_STRING("stepper", catfishMechName(CATFISH_MECH_STEPPER));
}

void test_mech_name_falls_back_to_motor(void) {
  TEST_ASSERT_EQUAL_STRING("motor", catfishMechName(200));
  TEST_ASSERT_EQUAL_STRING("motor", catfishMechName(0xFF));
}

// ---------------------------------------------------------------- fromName
void test_mech_from_name_roundtrip(void) {
  TEST_ASSERT_EQUAL_UINT8(CATFISH_MECH_MOTOR, catfishMechFromName("motor"));
  TEST_ASSERT_EQUAL_UINT8(CATFISH_MECH_SERVO, catfishMechFromName("servo"));
  TEST_ASSERT_EQUAL_UINT8(CATFISH_MECH_STEPPER, catfishMechFromName("stepper"));
}

void test_mech_from_name_rejects_unknown(void) {
  TEST_ASSERT_EQUAL_UINT8(0xFF, catfishMechFromName("hydraulic"));
  TEST_ASSERT_EQUAL_UINT8(0xFF, catfishMechFromName(""));
  TEST_ASSERT_EQUAL_UINT8(0xFF, catfishMechFromName(nullptr));
  // case-sensitive by design (curl examples in docs are lowercase)
  TEST_ASSERT_EQUAL_UINT8(0xFF, catfishMechFromName("Servo"));
}

// ---------------------------------------------------------------- clamps
void test_clamp_servo_deg(void) {
  TEST_ASSERT_EQUAL_UINT8(0, catfishClampServoDeg(-5));
  TEST_ASSERT_EQUAL_UINT8(0, catfishClampServoDeg(0));
  TEST_ASSERT_EQUAL_UINT8(90, catfishClampServoDeg(90));
  TEST_ASSERT_EQUAL_UINT8(180, catfishClampServoDeg(180));
  TEST_ASSERT_EQUAL_UINT8(180, catfishClampServoDeg(255));
}

void test_clamp_step_hz(void) {
  TEST_ASSERT_EQUAL_UINT16(1, catfishClampStepHz(0));
  TEST_ASSERT_EQUAL_UINT16(1, catfishClampStepHz(-100));
  TEST_ASSERT_EQUAL_UINT16(400, catfishClampStepHz(400));
  TEST_ASSERT_EQUAL_UINT16(1000, catfishClampStepHz(1000));
  TEST_ASSERT_EQUAL_UINT16(1000, catfishClampStepHz(5000));
}

void test_clamp_step_dir(void) {
  TEST_ASSERT_EQUAL_UINT8(0, catfishClampStepDir(0));
  TEST_ASSERT_EQUAL_UINT8(1, catfishClampStepDir(1));
  TEST_ASSERT_EQUAL_UINT8(1, catfishClampStepDir(42)); // any nonzero = 1
}

// ---------------------------------------------------------------- defaults
void test_defaults_sane(void) {
  // defaults must be inside the clamp ranges (boot-time values)
  TEST_ASSERT_TRUE(CATFISH_SERVO_REST_DEFAULT >= CATFISH_SERVO_DEG_MIN &&
                   CATFISH_SERVO_REST_DEFAULT <= CATFISH_SERVO_DEG_MAX);
  TEST_ASSERT_TRUE(CATFISH_SERVO_FEED_DEFAULT >= CATFISH_SERVO_DEG_MIN &&
                   CATFISH_SERVO_FEED_DEFAULT <= CATFISH_SERVO_DEG_MAX);
  TEST_ASSERT_TRUE(CATFISH_STEP_HZ_DEFAULT >= CATFISH_STEP_HZ_MIN &&
                   CATFISH_STEP_HZ_DEFAULT <= CATFISH_STEP_HZ_MAX);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_mech_names_canonical);
  RUN_TEST(test_mech_name_falls_back_to_motor);
  RUN_TEST(test_mech_from_name_roundtrip);
  RUN_TEST(test_mech_from_name_rejects_unknown);
  RUN_TEST(test_clamp_servo_deg);
  RUN_TEST(test_clamp_step_hz);
  RUN_TEST(test_clamp_step_dir);
  RUN_TEST(test_defaults_sane);
  return UNITY_END();
}
