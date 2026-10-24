/**
 * @file prefs.h
 * @brief Preferences (NVS) persistence wrapper for the Catfish Feeder display unit.
 *
 * Namespace "catfish". Load-on-boot via prefs::begin(), save-on-change via the
 * setters below (each setter writes NVS immediately and updates the RAM cache).
 * Getters return cached values, so they are cheap enough to call from loop().
 *
 * Threading: setters are called only from loop() (single writer). The network
 * worker task reads getRelayToken() concurrently — token access is internally
 * guarded, all other getters are benign snapshot reads.
 */
#ifndef CATFISH_PREFS_H
#define CATFISH_PREFS_H

#include <Arduino.h>
#include <stdint.h>

namespace prefs {

/**
 * @brief Open NVS namespace "catfish" and load all values into cache.
 * @return false if NVS could not be opened (defaults still usable, but
 *         setters will be no-ops on the flash side).
 */
bool begin();

/** @return true after a successful begin(). */
bool isLoaded();

// ---- Feeding schedule blob (owned by feed_scheduler, JSON array string) ----
String getScheduleJson();
/** @return false when the blob exceeds the cache size (rejected, not truncated). */
bool setScheduleJson(const String &json);

// ---- Relay unit pairing ----
/** LAN pairing token sent with every /feed POST (default "catfish"). */
String getRelayToken();
/** Store a new pairing token (clamped to CATFISH_TOKEN_LEN-1 chars). */
void setRelayToken(const String &token);
/**
 * Legacy single-relay IP slot — now an alias for tank 1's IP (multi-tank,
 * Phase 5). Kept for dashboard/older callers; new code uses getTankIp(1).
 */
String getRelayIp();
void setRelayIp(const String &ip);

// ---- Multi-tank (Phase 5): up to CATFISH_MAX_TANKS relays per display ----
// Tank ids are 1-based (tank 1 = the legacy relay). Per-tank token "" means
// "inherit the global pairing token". All setters clamp + persist to NVS;
// call them from loop() only (single-writer discipline).
uint8_t getTankCount(); // 1..CATFISH_MAX_TANKS
void setTankCount(uint8_t count);
String getTankName(uint8_t tank1); // 1-based; never empty (defaults filled)
void setTankName(uint8_t tank1, const String &name);
/** Per-tank token override ("" = inherit global token). */
String getTankToken(uint8_t tank1);
void setTankToken(uint8_t tank1, const String &token);
/** Last-known relay IP for that tank, "" = never discovered. */
String getTankIp(uint8_t tank1);
void setTankIp(uint8_t tank1, const String &ip);
/** Effective credential for a tank: per-tank override or the global token. */
String effectiveTankToken(uint8_t tank1);

// ---- Feeding defaults ----
/** Default manual-feed pulse in seconds (1..30). */
uint8_t getDefaultPulse();
void setDefaultPulse(uint8_t seconds);

// ---- MQTT / Home Assistant (display unit is the hub) ----
// All MQTT getters/setters are called ONLY from loop() (single-writer,
// same discipline as every other prefs section): applyConfig() runs in loop
// and hands copies to the MQTT client's internal task.
bool getMqttEnabled();
void setMqttEnabled(bool on);
/** Broker host or IP ("" = not configured). */
String getMqttHost();
void setMqttHost(const String &host);
uint16_t getMqttPort();
void setMqttPort(uint16_t port);
String getMqttUser();
void setMqttUser(const String &user);
String getMqttPass();
void setMqttPass(const String &pass);
/** State/command topic root (default "catfish"). */
String getMqttPrefix();
void setMqttPrefix(const String &prefix);

// ---- Display ----
/** User's backlight preference (idle dimming is transient, not persisted). */
bool getBacklightOn();
void setBacklightOn(bool on);

// ---- History ----
/** Unix epoch (UTC) of last successful feed, 0 = never. */
uint32_t getLastFedEpoch();
void setLastFedEpoch(uint32_t epoch);

} // namespace prefs

#endif // CATFISH_PREFS_H
