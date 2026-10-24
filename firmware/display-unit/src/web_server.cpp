/**
 * @file web_server.cpp
 * @brief Built-in web dashboard + JSON API (WebServer on its own task).
 *
 * Endpoints:
 *   GET  /                gzip single-page dashboard (PROGMEM)
 *   GET  /api/status      state snapshot JSON (open, LAN-only)
 *   GET  /api/version     {"version","proto"}
 *   GET  /api/schedule    schedule JSON array (open)
 *   PUT  /api/schedule    token; 400 bad JSON/size, 202 accepted (loop applies)
 *   POST /api/feed        token; {"seconds":1-30} optional; 202 queued
 *   POST /api/update      token; multipart firmware upload -> Update.h
 */
#include "web_server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <WebServer.h>
#include <freertos/queue.h>

#include <catfish_protocol.h>

#include "dashboard_html.h"
#include "mqtt_payloads.h"
#include "net_wifi.h"
#include "prefs.h"
#include "ui/ui.h"

namespace {

WebServer s_srv(80);
web_server::Snapshot s_snap; // loop() writes (publishSnapshot), web task reads
QueueHandle_t s_schedq = nullptr;
bool s_started = false; // listen socket up (needs WiFi first)
bool s_updating = false;

// ------------------------------------------------------------------ helpers
bool authOk() {
  return s_srv.hasHeader("X-Catfish-Token") &&
         s_srv.header("X-Catfish-Token") == prefs::getRelayToken();
}

void sendErr(int code, const char *msg) {
  s_srv.send(code, "application/json",
             String("{\"ok\":false,\"error\":\"") + msg + "\"}");
}

// ----------------------------------------------------------------- handlers
void handleRoot() {
  s_srv.sendHeader("Content-Encoding", "gzip");
  s_srv.sendHeader("Cache-Control", "no-cache");
  s_srv.send_P(200, "text/html", (const char *)DASHBOARD_HTML_GZ,
               DASHBOARD_HTML_GZ_LEN);
}

void handleStatus() {
  JsonDocument doc;
  doc["wifiUp"] = s_snap.wifiUp;
  doc["wifiSsid"] = s_snap.wifiSsid;
  doc["wifiIp"] = s_snap.wifiIp;
  doc["rssi"] = s_snap.rssi;
  doc["relayOnline"] = s_snap.relayOnline;
  doc["relayIp"] = s_snap.relayIp;
  doc["feeding"] = s_snap.feeding;
  doc["nextFeed"] = s_snap.nextFeed;
  doc["lastFedEpoch"] = s_snap.lastFedEpoch;
  doc["lastFeedIso"] = mqtt_payloads::epochToIso8601(s_snap.lastFedEpoch);
  doc["pulse"] = s_snap.pulse;
  doc["schedCount"] = s_snap.schedCount;
  doc["clockValid"] = s_snap.clockValid;
  doc["version"] = s_snap.version;
  doc["mqtt"] = s_snap.mqtt;
  doc["mqttState"] = s_snap.mqttState;
  // P5: per-tank status array (first tankCount entries are valid)
  uint8_t count = s_snap.tankCount;
  if (count < 1) count = 1;
  if (count > CATFISH_MAX_TANKS) count = CATFISH_MAX_TANKS;
  doc["selectedTank"] = s_snap.selectedTank;
  JsonArray tanks = doc["tanks"].to<JsonArray>();
  for (uint8_t i = 0; i < count; i++) {
    JsonObject t = tanks.add<JsonObject>();
    t["id"] = i + 1;
    t["name"] = s_snap.tankName[i];
    t["online"] = s_snap.tankOnline[i];
    t["feeding"] = s_snap.tankFeeding[i];
    t["ip"] = s_snap.tankIp[i];
  }
  String out;
  serializeJson(doc, out);
  s_srv.send(200, "application/json", out);
}

void handleVersion() {
  JsonDocument doc;
  doc["version"] = CATFISH_APP_VERSION;
  doc["proto"] = CATFISH_PROTOCOL_VERSION;
  String out;
  serializeJson(doc, out);
  s_srv.send(200, "application/json", out);
}

void handleScheduleGet() {
  // prefs getter = snapshot read of the same blob the scheduler persists ~1 Hz.
  s_srv.send(200, "application/json", prefs::getScheduleJson());
}

void handleSchedulePut() {
  if (!authOk()) return sendErr(403, "auth");
  const String body = s_srv.arg("plain");
  if (body.length() == 0 || body.length() >= 1024) return sendErr(400, "bad_size");
  // Parse-sanity only here; the authoritative all-or-nothing validation runs
  // in loop() (feed_scheduler::replaceAll).
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok ||
      !doc.is<JsonArray>()) {
    return sendErr(400, "bad_json");
  }
  char *copy = (char *)malloc(body.length() + 1);
  if (!copy) return sendErr(503, "busy");
  memcpy(copy, body.c_str(), body.length() + 1);
  if (xQueueSend(s_schedq, &copy, 0) != pdTRUE) {
    free(copy);
    return sendErr(503, "busy");
  }
  s_srv.send(202, "application/json", "{\"ok\":true,\"accepted\":true}");
}

void handleFeed() {
  if (!authOk()) return sendErr(403, "auth");
  uint8_t secs = 0;   // 0 -> device default pulse
  long tankMask = -1; // -1 -> all configured tanks
  const String body = s_srv.arg("plain");
  if (body.length()) {
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok)
      return sendErr(400, "bad_json");
    if (!doc["seconds"].isNull()) {
      const long v = doc["seconds"] | -1L;
      if (v < 1 || v > (long)CATFISH_PULSE_S_LIMIT) return sendErr(400, "bad_seconds");
      secs = (uint8_t)v;
    }
    if (!doc["tanks"].isNull()) {
      tankMask = doc["tanks"] | 0L;
      if (tankMask < 1 || tankMask > 15) return sendErr(400, "bad_tanks");
    }
  }
  // P5: default mask covers ALL configured tanks; an explicit "tanks" value
  // narrows it. The dashboard always sends an explicit mask anyway.
  uint8_t count = prefs::getTankCount();
  if (count < 1) count = 1;
  if (count > CATFISH_MAX_TANKS) count = CATFISH_MAX_TANKS;
  uint8_t mask = (count >= 4) ? 0x0F : (uint8_t)((1u << count) - 1u);
  if (tankMask > 0) mask = (uint8_t)tankMask;
  UiEvent ev;
  ev.type = UI_EVT_FEED;
  ev.flag = true; // manual (shows the on-device "Feeding fish..." toast too)
  ev.a = secs ? secs : prefs::getDefaultPulse();
  ev.tankMask = mask;
  if (!ui::ui_event_send(ev)) return sendErr(503, "busy");
  s_srv.send(202, "application/json", "{\"ok\":true,\"queued\":true}");
}

// ------------------------------------------------------------ firmware OTA
void updUploadCb() {
  HTTPUpload &up = s_srv.upload();
  if (up.status == UPLOAD_FILE_START) {
    // Token gate BEFORE Update.begin(): without a valid token we never write
    // a single flash byte.
    if (!authOk()) {
      Serial.println("[WEB] OTA upload rejected: bad token");
      s_updating = false;
      return;
    }
    s_updating = true;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Serial.printf("[WEB] OTA begin failed: %s\n", Update.errorString());
      s_updating = false;
    }
  } else if (up.status == UPLOAD_FILE_WRITE && s_updating) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      Serial.printf("[WEB] OTA write failed: %s\n", Update.errorString());
      Update.abort();
      s_updating = false;
    }
  } else if (up.status == UPLOAD_FILE_END && s_updating) {
    if (!Update.end(true)) {
      Serial.printf("[WEB] OTA end failed: %s\n", Update.errorString());
      s_updating = false;
    }
  }
}

void handleUpdatePost() {
  if (s_updating && !Update.hasError()) {
    s_srv.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    Serial.println("[WEB] OTA via dashboard complete - rebooting");
    delay(300);
    ESP.restart();
  } else {
    Update.abort();
    s_updating = false;
    sendErr(Update.hasError() ? 500 : 403, Update.hasError() ? "ota_error" : "auth");
  }
}

// -------------------------------------------------------------------- task
void webTask(void * /*param*/) {
  const char *hdrs[] = {"X-Catfish-Token"};
  s_srv.collectHeaders(hdrs, 1);

  s_srv.on("/", HTTP_GET, handleRoot);
  s_srv.on("/api/status", HTTP_GET, handleStatus);
  s_srv.on("/api/version", HTTP_GET, handleVersion);
  s_srv.on("/api/schedule", HTTP_GET, handleScheduleGet);
  s_srv.on("/api/schedule", HTTP_PUT, handleSchedulePut);
  s_srv.on("/api/feed", HTTP_POST, handleFeed);
  s_srv.on("/api/update", HTTP_POST, handleUpdatePost, updUploadCb);
  s_srv.onNotFound([] { sendErr(404, "not_found"); });

  for (;;) {
    if (!s_started) {
      if (net_wifi::wifi_is_connected()) {
        s_srv.begin(); // void on ESP32; listen socket follows WiFi up
        s_started = true;
        Serial.printf("[WEB] dashboard ready: http://%s/\n", s_snap.wifiIp);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    s_srv.handleClient();
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

} // anonymous namespace

namespace web_server {

bool begin() {
  if (s_schedq) return true;
  s_schedq = xQueueCreate(2, sizeof(char *));
  if (!s_schedq) {
    Serial.println("[WEB] ERROR: schedule queue alloc failed");
    return false;
  }
  // 12 KB: WebServer header parsing + multipart upload + JSON bodies.
  if (xTaskCreatePinnedToCore(webTask, "web-task", 12288, nullptr, 1, nullptr,
                              0) != pdPASS) {
    Serial.println("[WEB] ERROR: web task create failed");
    return false;
  }
  Serial.println("[WEB] web-task started (core 0)");
  return true;
}

void publishSnapshot(const Snapshot &snap) { s_snap = snap; }

bool takeScheduleReplace(char **json) {
  if (!s_schedq) return false;
  return xQueueReceive(s_schedq, json, 0) == pdTRUE;
}

} // namespace web_server
