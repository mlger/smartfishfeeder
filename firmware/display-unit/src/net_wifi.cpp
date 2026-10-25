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

// ---- v1.10.0: on-display WiFi manager (UI = single writer, wifi-task consumes)
constexpr uint8_t WIFI_MGR_MAX_APS = 12;
struct ApSnap {
  char ssid[33] = {0};
  int8_t rssi = 0;
  bool secure = false;
};
ApSnap s_aps[WIFI_MGR_MAX_APS];
volatile uint8_t s_ap_n = 0;
volatile uint8_t s_scan_req = 0;   // 1 = scan requested by the UI
volatile int s_scan_state = 0;     // WIFI_SCAN_*
volatile uint8_t s_conn_req = 0;   // 1 = connect requested by the UI
volatile int s_conn_state = 0;     // WIFI_CONN_*
char s_conn_ssid[33] = {0};
char s_conn_pass[65] = {0};

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
    // ---- v1.10.0: on-display WiFi manager commands (run here, not in UI) ----
    if (s_conn_req) {
      s_conn_req = 0;
      s_conn_state = net_wifi::CONN_BUSY;
      Serial.printf("[NET] UI connect request: '%s'\n", s_conn_ssid);
      WiFi.disconnect(false, false);
      vTaskDelay(pdMS_TO_TICKS(150));
      WiFi.begin(s_conn_ssid, s_conn_pass[0] ? s_conn_pass : nullptr);
      const uint32_t t0 = millis();
      bool ok = false;
      while ((int32_t)(millis() - t0) < 25000) {
        if (WiFi.status() == WL_CONNECTED) {
          ok = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
      }
      if (ok) {
        applyConnectedState();
        publish();
        s_connected = true;
        down_since = 0;
        Serial.printf("[NET] UI connect OK SSID='%s' IP=%s\n", s_ssid, s_ip);
      } else {
        // let the normal reconnect/portal machinery take back over
        down_since = millis();
        Serial.println("[NET] UI connect FAILED");
      }
      s_conn_state = ok ? net_wifi::CONN_OK : net_wifi::CONN_FAILED;
    }

    if (s_scan_req) {
      s_scan_req = 0;
      s_scan_state = net_wifi::SCAN_BUSY;
      Serial.println("[NET] UI scan request");
      // v1.10.1 FIX ("scanning of wifi failed"): esp_wifi_scan_start() is
      // rejected with ESP_ERR_WIFI_STATE while the STA is mid-(re)connect,
      // and WiFiScanClass then keeps reporting WIFI_SCAN_FAILED without ever
      // setting its SCANNING flag — the UI showed "scan failed" exactly when
      // the user needs the picker (device down, auto-reconnect in flight,
      // first seconds after boot). Detach any in-flight connect attempt first
      // (NVS credentials survive disconnect(false,false)), retry the start a
      // few times, and honor the driver's answer instead of blind-polling.
      if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(false, false); // abort connecting; STA stays enabled
        vTaskDelay(pdMS_TO_TICKS(120));
      }
      bool scanning = false;
      for (uint8_t tries = 0; tries < 3 && !scanning; tries++) {
        if (tries) {
          vTaskDelay(pdMS_TO_TICKS(250));
          if (WiFi.status() != WL_CONNECTED) WiFi.disconnect(false, false);
        }
        scanning = (WiFi.scanNetworks(true /* async */) ==
                    (int16_t)WIFI_SCAN_RUNNING);
      }
      int done = scanning ? (int)WIFI_SCAN_RUNNING : (int)WIFI_SCAN_FAILED;
      if (scanning) {
        const uint32_t t0 = millis();
        while ((int32_t)(millis() - t0) < 12000) {
          done = WiFi.scanComplete();
          if (done != WIFI_SCAN_RUNNING) break;
          vTaskDelay(pdMS_TO_TICKS(250));
        }
      }
      if (done >= 0) {
        uint8_t n = 0;
        for (int i = 0; i < done && n < WIFI_MGR_MAX_APS; i++) {
          String ssid = WiFi.SSID(i);
          if (!ssid.length()) continue; // skip hidden/blank entries
          strlcpy(s_aps[n].ssid, ssid.c_str(), sizeof(s_aps[n].ssid));
          s_aps[n].rssi = (int8_t)WiFi.RSSI(i);
          s_aps[n].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
          n++;
        }
        s_ap_n = n;
        WiFi.scanDelete();
        s_scan_state = net_wifi::SCAN_DONE;
        Serial.printf("[NET] UI scan done: %u AP(s)\n", (unsigned)n);
      } else {
        s_scan_state = net_wifi::SCAN_FAILED;
        Serial.println("[NET] UI scan FAILED/timeout");
      }
    }

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

// -------------------------------------------------- v1.10.0: WiFi manager
void requestScan() {
  if (s_scan_req || s_scan_state == net_wifi::SCAN_BUSY) return;
  s_scan_state = net_wifi::SCAN_BUSY; // immediate feedback; task confirms
  s_scan_req = 1;
}

void requestConnect(const String &ssid, const String &pass) {
  if (!ssid.length()) return;
  strlcpy(s_conn_ssid, ssid.c_str(), sizeof(s_conn_ssid));
  strlcpy(s_conn_pass, pass.c_str(), sizeof(s_conn_pass));
  s_conn_state = net_wifi::CONN_BUSY;
  s_conn_req = 1;
}

int scanState() { return s_scan_state; }

uint8_t scanCount() { return s_ap_n; }

bool scanItem(uint8_t i, String &ssid, int &rssi, bool &secure) {
  if (i >= WIFI_MGR_MAX_APS || i >= s_ap_n) return false;
  ssid = String(s_aps[i].ssid);
  rssi = s_aps[i].rssi;
  secure = s_aps[i].secure;
  return true;
}

int connectState() { return s_conn_state; }

String connectSsid() { return String(s_conn_ssid); }

} // namespace net_wifi
