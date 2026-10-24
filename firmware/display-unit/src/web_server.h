/**
 * @file web_server.h
 * @brief Built-in web dashboard + JSON API for the display unit.
 *
 * Runs a WebServer (port 80) on a dedicated FreeRTOS task ("web-task", core 0,
 * prio 1). The task NEVER touches LVGL, the scheduler, or prefs setters:
 *   - reads come from a plain-field Snapshot cache that loop() refreshes 1 Hz,
 *   - mutations (feed, schedule replace) are enqueued: feed via the shared
 *     UiEvent queue (same single-writer path as touch input), schedule via a
 *     small pointer queue drained by loop() (authoritative replaceAll there).
 *
 * Auth: mutating endpoints require the pairing token in the X-Catfish-Token
 * header (same token the relay unit checks). GET endpoints are open — the
 * appliance is LAN-only by design (documented).
 */
#ifndef CATFISH_WEB_SERVER_H
#define CATFISH_WEB_SERVER_H

#include <Arduino.h>
#include <stdint.h>

#include <catfish_protocol.h>

namespace web_server {

/** Plain-field state cache: loop() writes, web task reads. No heap, no locks. */
struct Snapshot {
  bool wifiUp = false;
  char wifiSsid[33] = {0};
  char wifiIp[16] = {0};
  int16_t rssi = 0;
  bool relayOnline = false; // SELECTED tank (legacy aggregate field)
  bool feeding = false;     // any configured tank feeding
  char relayIp[16] = {0};
  char nextFeed[12] = {0}; // "HH:MM", "none", or "" while clock unsynced
  uint32_t lastFedEpoch = 0;
  uint8_t pulse = 0;
  uint16_t schedCount = 0;
  bool clockValid = false;
  char version[12] = {0};
  char mqtt[20] = {0};
  uint8_t mqttState = 0; // 0 disabled, 1 busy, 2 connected (dashboard dot)
  // Multi-tank (P5): first tankCount entries are valid
  uint8_t tankCount = 1;
  uint8_t selectedTank = 0; // 0-based; 0xFE = "All"
  char tankName[CATFISH_MAX_TANKS][CATFISH_TANK_NAME_LEN] = {};
  bool tankOnline[CATFISH_MAX_TANKS] = {false, false, false, false};
  bool tankFeeding[CATFISH_MAX_TANKS] = {false, false, false, false};
  char tankIp[CATFISH_MAX_TANKS][16] = {};
};

/** Create the schedule queue + spawn the web task. Call once from setup(). */
bool begin();

/** loop() context: refresh the state cache the web task serves from. */
void publishSnapshot(const Snapshot &snap);

/**
 * loop() context: pop one heap-allocated schedule JSON handed over by a
 * PUT /api/schedule. Caller MUST free() the buffer. @return false when empty.
 */
bool takeScheduleReplace(char **json);

} // namespace web_server

#endif // CATFISH_WEB_SERVER_H
