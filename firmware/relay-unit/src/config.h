/**
 * @file config.h
 * @brief Catfish Feeder — Relay Unit — build-time tunables.
 *
 * All knobs live here. Runtime-persisted settings (token, maxPulseS, pulseS)
 * live in EEPROM and are editable via POST /config; the values below are the
 * factory defaults / hard limits / hardware mapping.
 *
 * Protocol constants shared with the display unit (discovery strings/ports,
 * hostnames, token default/capacity, pulse limits, endpoint paths) come from
 * firmware/common/catfish_protocol.h — single source of truth for both units.
 */
#pragma once

#include <catfish_protocol.h>
#include <catfish_mech.h>

// --------------------------------------------------- shared protocol (aliases)
// Relay firmware keeps its historical macro names; values live in the shared
// header so both units can never drift apart.
#define DISCOVERY_UDP_PORT  CATFISH_DISCOVERY_UDP_PORT
#define DISCOVERY_REQ       CATFISH_DISCOVERY_REQ
#define DISCOVERY_RSP_PREFIX CATFISH_DISCOVERY_RSP_PREFIX
#define HOSTNAME            CATFISH_RELAY_HOSTNAME
#define DEFAULT_TOKEN       CATFISH_DEFAULT_TOKEN
#define RELAY_TOKEN_LEN     CATFISH_TOKEN_LEN
#define MAX_PULSE_LIMIT_S   CATFISH_MAX_PULSE_LIMIT_S
#define PULSE_S_LIMIT       CATFISH_PULSE_S_LIMIT
#define DEFAULT_PULSE_S     CATFISH_DEFAULT_PULSE_S

// ---------------------------------------------------------------- firmware
// Derived from the shared protocol header — single version source for both units.
#define FIRMWARE_VERSION CATFISH_APP_VERSION

// ------------------------------------------------------------------ relay
// D1 == GPIO5 on NodeMCU (feeds the optocoupler input of the relay module).
//
// RELAY_ACTIVE_LEVEL: most cheap 5V 1-channel optocoupler relay modules are
// ACTIVE-LOW (the opto LED sinks current to GND, so LOW = coil energized).
// >>> If your relay clicks ON at boot instead of staying off, flip this to
// >>> HIGH. main.cpp boots the pin INPUT_PULLUP + pre-loads the INACTIVE
// >>> level precisely so an active-LOW module stays OFF through boot noise.
#define RELAY_PIN          D1
#define RELAY_ACTIVE_LEVEL LOW

#define RELAY_ON           (RELAY_ACTIVE_LEVEL)
#define RELAY_OFF          (!RELAY_ACTIVE_LEVEL)

// ------------------------------------------------ feeding mechanism pins (P4)
// All three mechanisms keep the "feed for N seconds" semantics; only what the
// pins DO during those seconds differs (see catfish_mech.h + docs/firmware.md).
//
//   motor   (default) D1 relay/FET output ON for N s (existing wiring)
//   servo   D5 (GPIO14) signal wire of an SG90-class servo (red=5V, brown=GND)
//   stepper D2 (GPIO4)  -> driver STEP input
//           D6 (GPIO12) -> driver DIR input
//           (A4988/DRV8825: also tie /ENABLE+RESET+SLEEP per its datasheet,
//            MS1-MS3 set microstepping; VMOT decoupling cap >= 100 uF!)
#define SERVO_PIN          D5
#define STEP_PIN           D2
#define DIR_PIN            D6

// ------------------------------------------------------------- status LED
// LED_BUILTIN == D4/GPIO2 on NodeMCU V3, active LOW (sinks through the LED).
#define STATUS_LED         LED_BUILTIN
#define LED_ON             LOW
#define LED_OFF            HIGH

// LED behaviour tunables (ms)
#define LED_HEARTBEAT_PERIOD_MS 2000UL  // heartbeat blink interval
#define LED_HEARTBEAT_ON_MS     40UL    // heartbeat "blip" width
#define LED_FEED_BLINK_MS       100UL   // fast-blink half-period while feeding

// ---------------------------------------------------------- pulse safety
#define DEFAULT_MAX_PULSE_S 30 // per-device soft cap (EEPROM-editable via /config)

// ------------------------------------------------------------- OTA (ArduinoOTA)
// Open OTA is a documented decision for this LAN-only appliance. To require a
// password, add e.g.  -DOTA_PASSWORD=\"s3cret\"  to build_flags (or inject it
// from CI). Empty string keeps the historical open behaviour.
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

// ------------------------------------------------- factory reset (FLASH button)
// Hold the NodeMCU FLASH button (GPIO0) for this long while running to wipe
// the EEPROM config (token, pulse caps) back to factory defaults and reboot.
// Guarded against activation while a feed pulse is running.
#define FACTORY_RESET_BTN      0        // GPIO0 = FLASH button on NodeMCU
#define FACTORY_RESET_HOLD_MS  5000UL   // hold duration to trigger the wipe

// ------------------------------------------- EEPROM config blobs (see main.cpp)
#define EEPROM_SIZE        4096
#define CFG_MAGIC          0xC47F15EEUL  // config sentinel (0xC47F15"EE" — valid uint32)
// Phase 4: second, ADDITIVE record at its own offset — upgrading an existing
// install keeps the original token/pulse blob byte-for-byte intact.
#define MECH_EEPROM_OFFSET 64
#define MECH_MAGIC         0x4D454348UL  // "MECH" in ASCII

// -------------------------------------------------------------- identity
#define AP_NAME            "CatfishRelay-Setup" // WiFiManager fallback AP SSID

// ---------------------------------------------------------- wifi / portal
#define WM_PORTAL_TIMEOUT_S     180UL    // setup portal gives up after this, device reboots and retries
#define WIFI_RECONNECT_AFTER_MS 30000UL  // disconnected longer than this -> WiFi.reconnect()

// ------------------------------------------------------------------ time
// POSIX TZ string, sign inverted: "PHT-8" == UTC+8 (Philippines).
#define NTP_TZ       "PHT-8"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
