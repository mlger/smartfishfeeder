/**
 * @file mqtt_mgr.h
 * @brief MQTT connection manager for the display unit (Home Assistant hub).
 *
 * The espMqttClient runs in its own internal FreeRTOS task (prio 1, core 0).
 * Its callbacks fire on the async-TCP task context and therefore ONLY:
 *   - enqueue connect/disconnect events (drained by loop()),
 *   - subscribe command topics,
 *   - route incoming commands into the shared UiEvent queue (never touch
 *     LVGL, prefs setters, or the scheduler — single-writer contract).
 *
 * loop() responsibilities (all here are loop-context only):
 *   - applyConfig(): re-read prefs, (re)configure + start/stop the client
 *   - retryIfDue(): reconnect with exponential backoff (5 s -> 60 s cap)
 *   - publish*(): state publishing (non-blocking, no-ops when disconnected)
 *
 * Design note: the relay unit deliberately stays HTTP-only — the display is
 * the single MQTT hub, so the ESP8266 budget is untouched.
 */
#ifndef CATFISH_MQTT_MGR_H
#define CATFISH_MQTT_MGR_H

#include <Arduino.h>
#include <stdint.h>

namespace mqtt_mgr {

enum class Status : uint8_t {
  Disabled = 0, // user switch off (or broker not configured)
  Offline,      // enabled, waiting to retry (backoff)
  Connecting,   // TCP/MQTT handshake in flight
  Connected
};

/** Create the client + event queue. Call once from setup(). */
bool begin();

/**
 * Re-read prefs and (re)configure the client. Called from loop() after the
 * user saves MQTT settings (UI_EVT_MQTT_CFG) and once at boot. Disconnects
 * a live session first when the config changed.
 */
void applyConfig();

/** Loop-context reconnect pump (call ~1 Hz). WiFi-gated, backoff-based. */
void retryIfDue(uint32_t nowMs);

/** Non-blocking connection-event drain for loop(). @return false when empty. */
bool receive(bool &connected);

Status status();
bool connected();

// ---- state publishing (loop context; no-ops when disconnected) ------------
/** Publish all retained discovery configs (call once after every connect). */
void publishDiscovery();
/** Retained availability "online" (the LWT publishes "offline" on drop). */
void publishAvailable();
/** Full state snapshot after (re)connect; values come from loop, not tasks. */
void publishLastFeed(uint32_t epoch);
void publishNextFeed(const String &hhmm);
void publishRelayOnline(bool online);
void publishFeeding(bool feeding);
void publishPulse(uint8_t seconds);

// ---- per-tank entities (Phase 5) -------------------------------------------
/** Retained per-tank online state ("ON"/"OFF") for tank @p tank1 (1-based). */
void publishTankOnline(uint8_t tank1, bool online);
/**
 * After the configured tank count shrinks: remove the HA discovery configs
 * (empty retained payloads) and clear state topics for tanks that no longer
 * exist. No-op ranges are handled internally.
 */
void clearTankDiscovery(uint8_t prevCount, uint8_t newCount);

} // namespace mqtt_mgr

#endif // CATFISH_MQTT_MGR_H
