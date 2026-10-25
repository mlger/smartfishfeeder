/**
 * @file net_wifi.h
 * @brief WiFiManager wrapper + on-display WiFi manager (v1.10.0).
 *
 * Runs WiFiManager.autoConnect("CatfishFeeder-Setup") in a dedicated
 * FreeRTOS task ("wifi-task", core 0) so the LVGL task stays responsive.
 * After connect: WiFi sleep off, MDNS "catfishfeeder", NTP Asia/Manila.
 * Monitors link every 5 s; auto-reconnects; reopens the config portal
 * (180 s timeout) after >90 s of continuous downtime.
 *
 * v1.10.0 on-display WiFi manager: the touch UI can trigger an async AP scan
 * (requestScan/scanState/scanItem) and connect to a picked AP with a password
 * (requestConnect/connectState). All commands run on wifi-task; the UI polls
 * small state ints + snapshots — no cross-task WiFi calls.
 */
#ifndef CATFISH_NET_WIFI_H
#define CATFISH_NET_WIFI_H

#include <Arduino.h>

namespace net_wifi {

/** Scan/connect lifecycle states shared with the UI. NOTE: deliberately NOT
 *  named WIFI_SCAN_* — the Arduino core #defines WIFI_SCAN_RUNNING/FAILED as
 *  macros, which would shred any enum using those names. */
enum Lifecycle : int {
  SCAN_IDLE = 0,
  SCAN_BUSY = 1,
  SCAN_DONE = 2,
  SCAN_FAILED = 3,
  CONN_IDLE = 0,
  CONN_BUSY = 1,
  CONN_OK = 2,
  CONN_FAILED = 3,
};

/** Spawn the wifi task. Call once from setup(). */
void start();

bool wifi_is_connected();
String wifi_ip();
String wifi_ssid();
int wifi_rssi();

// -------------------------------------------------- v1.10.0: WiFi manager
/** Queue an async AP scan (no-op while one is already running/queued). */
void requestScan();
/** Queue a connect to @p ssid with @p pass (clears the last result). */
void requestConnect(const String &ssid, const String &pass);

int scanState();
/** Number of scan results snapshotted after the last completed scan. */
uint8_t scanCount();
/** Copy scan result @p i (0-based) into the caller's fields. */
bool scanItem(uint8_t i, String &ssid, int &rssi, bool &secure);

int connectState();
/** SSID of the last requested connect ("" = none). */
String connectSsid();

} // namespace net_wifi

#endif // CATFISH_NET_WIFI_H
