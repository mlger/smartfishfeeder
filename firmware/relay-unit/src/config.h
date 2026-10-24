/**
 * @file config.h
 * @brief Catfish Feeder — Relay Unit — build-time tunables.
 *
 * All knobs live here. Runtime-persisted settings (token, maxPulseS, pulseS)
 * live in EEPROM and are editable via POST /config; the values below are the
 * factory defaults / hard limits / hardware mapping.
 */
#pragma once

// ---------------------------------------------------------------- firmware
#define FIRMWARE_VERSION "1.0.0"

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

// ------------------------------------------------------------- status LED
// LED_BUILTIN == D4/GPIO2 on NodeMCU V3, active LOW (sinks through the LED).
#define STATUS_LED         LED_BUILTIN
#define LED_ON             LOW
#define LED_OFF            HIGH

// LED behaviour tunables (ms)
#define LED_HEARTBEAT_PERIOD_MS 2000UL  // heartbeat blink interval
#define LED_HEARTBEAT_ON_MS     40UL    // heartbeat "blip" width
#define LED_FEED_BLINK_MS       100UL   // fast-blink half-period while feeding

// ------------------------------------------------- feed auth (network API)
// DEFAULT_TOKEN is the SHIPPED PRODUCT DEFAULT, deliberately not a secret:
// it only gates feed/config calls on the LAN. Change it on-site via
// POST /config {"token":"<current>", "newToken":"<new>"}.
// It is never written to any other file and never logged.
#define DEFAULT_TOKEN      "catfish"
#define RELAY_TOKEN_LEN    24  // storage capacity incl. NUL (EEPROM layout)

// ---------------------------------------------------------- pulse safety
#define MAX_PULSE_LIMIT_S  60  // HARD cap — clamp target for any feed request
#define DEFAULT_MAX_PULSE_S 30 // per-device soft cap (EEPROM-editable via /config)
#define DEFAULT_PULSE_S    3   // default pulse duration (display unit may mirror it)
#define PULSE_S_LIMIT      30  // max allowed value of pulseS in /config

// ------------------------------------------- EEPROM config blob (see main.cpp)
#define EEPROM_SIZE        4096
#define CFG_MAGIC          0xC47F15EEUL  // config sentinel (0xC47F15"EE" — valid uint32)

// -------------------------------------------- discovery (LAN, UDP broadcast)
#define DISCOVERY_UDP_PORT 5050
#define DISCOVERY_REQ      "CATFISH_DISCOVER"   // request string, exact match
#define DISCOVERY_RSP_PREFIX "CATFISH_RELAY|"   // reply: prefix + <local IP>

// -------------------------------------------------------------- identity
#define HOSTNAME           "catfish-relay"      // mDNS + DHCP hostname + OTA name
#define AP_NAME            "CatfishRelay-Setup" // WiFiManager fallback AP SSID

// ---------------------------------------------------------- wifi / portal
#define WM_PORTAL_TIMEOUT_S     180UL    // setup portal gives up after this, device reboots and retries
#define WIFI_RECONNECT_AFTER_MS 30000UL  // disconnected longer than this -> WiFi.reconnect()

// ------------------------------------------------------------------ time
// POSIX TZ string, sign inverted: "PHT-8" == UTC+8 (Philippines).
#define NTP_TZ       "PHT-8"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
