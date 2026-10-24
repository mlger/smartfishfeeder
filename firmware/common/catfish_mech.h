/**
 * @file catfish_mech.h
 * @brief Catfish Feeder — feeding-mechanism constants + pure helpers.
 *
 * The relay unit supports three feeding mechanisms (Phase 4 roadmap item).
 * ALL of them keep the same wire semantics: "feed for N seconds" — only what
 * the ESP8266 does with those seconds differs:
 *
 *   motor   (default) relay/FET output on for N seconds (DC auger motor)
 *   servo             SG90-class servo sweeps between rest and feed angle for
 *                     N seconds (0.5 s per direction), parks at rest after
 *   stepper           A4988/DRV8825-class driver: STEP pulses at stepHz for
 *                     N seconds, DIR held at the configured level
 *
 * Dependency-free (no Arduino includes) so both the ESP8266 relay firmware
 * and the host-side [env:native] unit tests can use it. The relay clamps
 * every /config value through these same helpers, so display, relay and
 * tests can never drift apart.
 */
#pragma once

#include <stdint.h>
#include <string.h>

// ------------------------------------------------------------ mechanism ids
#define CATFISH_MECH_MOTOR   0
#define CATFISH_MECH_SERVO   1
#define CATFISH_MECH_STEPPER 2
#define CATFISH_MECH_DEFAULT CATFISH_MECH_MOTOR
#define CATFISH_MECH_COUNT   3

// ---------------------------------------------------------------- limits
#define CATFISH_SERVO_DEG_MIN 0
#define CATFISH_SERVO_DEG_MAX 180
#define CATFISH_SERVO_REST_DEFAULT 0
#define CATFISH_SERVO_FEED_DEFAULT 90
#define CATFISH_STEP_HZ_MIN     1
#define CATFISH_STEP_HZ_MAX     1000
#define CATFISH_STEP_HZ_DEFAULT 400

// ------------------------------------------------------------ implementations
// Header-only inline: the same single definition is compiled into the relay
// firmware, the display unit and the host-side tests (no macro switches).
#ifndef __cplusplus
#error "catfish_mech.h is a C++ header (inline functions)"
#endif

inline const char *catfishMechName(uint8_t mech) {
  switch (mech) {
    case CATFISH_MECH_SERVO:   return "servo";
    case CATFISH_MECH_STEPPER: return "stepper";
    case CATFISH_MECH_MOTOR:
    default:                   return "motor";
  }
}

inline uint8_t catfishMechFromName(const char *name) {
  if (!name) return 0xFF;
  if (strcmp(name, "motor") == 0) return CATFISH_MECH_MOTOR;
  if (strcmp(name, "servo") == 0) return CATFISH_MECH_SERVO;
  if (strcmp(name, "stepper") == 0) return CATFISH_MECH_STEPPER;
  return 0xFF;
}

inline uint8_t catfishClampServoDeg(long deg) {
  if (deg < CATFISH_SERVO_DEG_MIN) return CATFISH_SERVO_DEG_MIN;
  if (deg > CATFISH_SERVO_DEG_MAX) return CATFISH_SERVO_DEG_MAX;
  return (uint8_t)deg;
}

inline uint16_t catfishClampStepHz(long hz) {
  if (hz < CATFISH_STEP_HZ_MIN) return CATFISH_STEP_HZ_MIN;
  if (hz > CATFISH_STEP_HZ_MAX) return CATFISH_STEP_HZ_MAX;
  return (uint16_t)hz;
}

inline uint8_t catfishClampStepDir(long dir) { return dir ? 1 : 0; }
