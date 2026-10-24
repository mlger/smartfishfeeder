/**
 * @file prefs.h
 * @brief Preferences (NVS) persistence wrapper for the Catfish Feeder display unit.
 *
 * Namespace "catfish". Load-on-boot via prefs::begin(), save-on-change via the
 * setters below (each setter writes NVS immediately and updates the RAM cache).
 * Getters return cached values, so they are cheap enough to call from loop().
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
void setScheduleJson(const String &json);

// ---- Relay unit pairing ----
/** LAN pairing token sent with every /feed POST (default "catfish"). */
String getRelayToken();
/** Last known relay IP as dotted string, "" if never discovered. */
String getRelayIp();
void setRelayIp(const String &ip);

// ---- Feeding defaults ----
/** Default manual-feed pulse in seconds (1..30). */
uint8_t getDefaultPulse();
void setDefaultPulse(uint8_t seconds);

// ---- History ----
/** Unix epoch (UTC) of last successful feed, 0 = never. */
uint32_t getLastFedEpoch();
void setLastFedEpoch(uint32_t epoch);

} // namespace prefs

#endif // CATFISH_PREFS_H
