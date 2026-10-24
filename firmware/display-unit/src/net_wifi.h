/**
 * @file net_wifi.h
 * @brief WiFiManager wrapper for the Catfish Feeder display unit.
 *
 * Runs WiFiManager.autoConnect("CatfishFeeder-Setup") in a dedicated
 * FreeRTOS task ("wifi-task", core 0) so the LVGL task stays responsive.
 * After connect: WiFi sleep off, MDNS "catfishfeeder", NTP Asia/Manila.
 * Monitors link every 5 s; auto-reconnects; reopens the config portal
 * (180 s timeout) after >90 s of continuous downtime.
 */
#ifndef CATFISH_NET_WIFI_H
#define CATFISH_NET_WIFI_H

#include <Arduino.h>

namespace net_wifi {

/** Spawn the wifi task. Call once from setup(). */
void start();

bool wifi_is_connected();
String wifi_ip();
String wifi_ssid();
int wifi_rssi();

} // namespace net_wifi

#endif // CATFISH_NET_WIFI_H
