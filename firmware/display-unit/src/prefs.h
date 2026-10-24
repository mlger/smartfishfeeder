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
/** Last known relay IP as dotted string, "" if never discovered. */
String getRelayIp();
void setRelayIp(const String &ip);

// ---- Feeding defaults ----
/** Default manual-feed pulse in seconds (1..30). */
uint8_t getDefaultPulse();
void setDefaultPulse(uint8_t seconds);

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
