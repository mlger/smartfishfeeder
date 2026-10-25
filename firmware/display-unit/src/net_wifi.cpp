/**
 * @file net_wifi.cpp
 * @brief WiFi connection manager task (WiFiManager + mDNS + NTP).
 */
#include "net_wifi.h"
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

#define WIFI_AP_NAME       "CatfishFeeder-Setup"
#define WIFI_HOSTNAME      "catfishfeeder"
#define WIFI_MONITOR_MS    5000
#define WIFI_PORTAL_AFTER_MS 90000  // reopen portal after this much downtime
#define WIFI_PORTAL_TIMEOUT_S 180

namespace {

// Shared with loop() via getters. Strings are char buffers so cross-task
// reads always see a valid NUL-terminated snapshot.
volatile bool s_connected = false;
char s_ip[16] = {0};
char s_ssid[33] = {0};
volatile int s_rssi = 0;
bool s_mdns_started = false;

void publish() {
  strlcpy(s_ip, WiFi.localIP().toString().c_str(), sizeof(s_ip));
  strlcpy(s_ssid, WiFi.SSID().c_str(), sizeof(s_ssid));
  s_rssi = WiFi.RSSI();
}

/** Called on every (re)connect: sleep off, mDNS (once), NTP Asia/Manila (UTC+8). */
void applyConnectedState() {
  WiFi.setSleep(false);
  if (!s_mdns_started) {
    // MDNS.begin() fails when already started; guard so reconnect logs stay honest.
    if (MDNS.begin(WIFI_HOSTNAME)) {
      s_mdns_started = true;
      Serial.println("[NET] mDNS started as 'catfishfeeder'");
    }
  }
  configTzTime("PHT-8", "pool.ntp.org", "time.nist.gov"); // Asia/Manila UTC+8
}

void wifiTask(void * /*param*/) {
  // v1.8.4 FIX (WiFi credentials lost on every power cycle): the old
  // WiFi.persistent(false) forced esp_wifi_set_storage(WIFI_STORAGE_RAM),
  // so the STA config never reached NVS and each boot opened the setup
  // portal again. With the default WIFI_STORAGE_FLASH the driver writes
  // the STA config into NVS only when the config CHANGES (i.e. once per
  // credential entry in the portal) — no meaningful NVS wear.
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.setAutoReconnect(true);

  static WiFiManager wm; // persists for portal restarts
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  wm.setConnectTimeout(15);
  wm.setHostname(WIFI_HOSTNAME);
  wm.setBreakAfterConfig(true);

  Serial.printf("[NET] autoConnect (AP '%s', portal %d s)...\n",
                WIFI_AP_NAME, WIFI_PORTAL_TIMEOUT_S);
  if (wm.autoConnect(WIFI_AP_NAME)) {
    applyConnectedState();
    publish();
    s_connected = true;
    Serial.printf("[NET] connected SSID='%s' IP=%s RSSI=%d\n",
                  s_ssid, s_ip, s_rssi);
  } else {
    Serial.println("[NET] initial connect failed; retry loop engaged");
  }

  uint32_t down_since = 0;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!s_connected) {
        applyConnectedState();
        Serial.println("[NET] (re)connected");
      }
      s_connected = true;
      down_since = 0;
      publish();
    } else {
      if (s_connected) {
        // just lost link
        s_connected = false;
        s_rssi = 0;
        down_since = millis();
        Serial.println("[NET] WiFi lost - reconnecting");
        WiFi.reconnect();
      } else if (down_since == 0) {
        down_since = millis();
      }

      if (down_since != 0 && (millis() - down_since) > WIFI_PORTAL_AFTER_MS) {
        Serial.println("[NET] down > 90 s - opening config portal (180 s)");
        const bool ok = wm.autoConnect(WIFI_AP_NAME);
        if (ok) {
          down_since = 0;
        } else {
          // retry portal after another 90 s of downtime
          down_since = millis();
          Serial.println("[NET] portal attempt failed; retrying later");
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(WIFI_MONITOR_MS));
  }
}

} // anonymous namespace

namespace net_wifi {

void start() {
  // Stack in BYTES on ESP-IDF/Arduino: 16 KB >= required 8 KB (WiFiManager
  // portal + DNS + webserver are stack hungry).
  xTaskCreatePinnedToCore(wifiTask, "wifi-task", 16384, nullptr, 3, nullptr, 0);
  Serial.println("[NET] wifi-task started (core 0)");
}

bool wifi_is_connected() {
  return s_connected && WiFi.status() == WL_CONNECTED;
}

String wifi_ip() { return String(s_ip); }

String wifi_ssid() { return String(s_ssid); }

int wifi_rssi() { return s_rssi; }

} // namespace net_wifi
