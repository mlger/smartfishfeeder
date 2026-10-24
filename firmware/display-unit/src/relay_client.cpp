/**
 * @file relay_client.cpp
 * @brief LAN HTTP + mDNS + UDP-broadcast client for the ESP8266 relay unit.
 */
#include "relay_client.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include "net_wifi.h"
#include "prefs.h"

namespace {

String s_ip; // cached relay IP, dotted string ("" = unknown)

bool validIpString(const String &s) {
  IPAddress t;
  return t.fromString(s) && s != String("0.0.0.0");
}

/**
 * Minimal HTTP helper.
 * @return HTTP status code (>=100) or negative transport error; body in resp.
 */
int httpReq(const String &url, bool post, const String &body,
            int timeout_ms, String &resp) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(timeout_ms);
  http.setTimeout(timeout_ms);
  if (!http.begin(client, url)) return -100;
  int code;
  if (post) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
  } else {
    code = http.GET();
  }
  if (code > 0) resp = http.getString();
  http.end();
  return code;
}

bool healthCheck(const String &ip, int timeout_ms) {
  String resp;
  int code = httpReq("http://" + ip + "/health", false, "", timeout_ms, resp);
  return (code >= 200 && code < 300);
}

} // anonymous namespace

namespace relay_client {

void init() {
  s_ip = prefs::getRelayIp();
  if (s_ip.length()) {
    Serial.printf("[NET] relay: saved IP %s (will validate on first use)\n",
                  s_ip.c_str());
  }
}

bool hasIp() { return s_ip.length() > 0; }

String ip() { return s_ip; }

Result discover() {
  Result r;

  if (!net_wifi::wifi_is_connected()) {
    r.error = "WiFi down";
    return r;
  }

  // (1) Saved IP validated by GET /health (2 s timeout)
  if (s_ip.length()) {
    if (healthCheck(s_ip, 2000)) {
      r.ok = true;
      r.ip = s_ip;
      Serial.printf("[NET] relay healthy at saved IP %s\n", s_ip.c_str());
      return r;
    }
    Serial.printf("[NET] saved relay IP %s not responding\n", s_ip.c_str());
  }

  // (2) mDNS query for "catfish-relay" (we advertise as "catfishfeeder")
  IPAddress resolved = MDNS.queryHost("catfish-relay", 2000);
  if (resolved != IPAddress(0, 0, 0, 0)) {
    String mIp = resolved.toString();
    if (validIpString(mIp)) {
      Serial.printf("[NET] mDNS resolved catfish-relay -> %s\n", mIp.c_str());
      s_ip = mIp;
      prefs::setRelayIp(mIp);
      r.ok = true;
      r.ip = mIp;
      return r;
    }
  }

  // (3) UDP broadcast discovery on port 5050
  WiFiUDP udp;
  if (udp.begin(5051)) { // local (source) port; relay replies here
    udp.beginPacket(IPAddress(255, 255, 255, 255), 5050);
    udp.print("CATFISH_DISCOVER\n");
    udp.endPacket();

    const uint32_t t0 = millis();
    while (millis() - t0 < 1500) {
      int p = udp.parsePacket();
      if (p > 0) {
        char buf[80] = {0};
        udp.read(buf, sizeof(buf) - 1);
        String line(buf);
        line.trim();
        Serial.printf("[NET] discovery reply: '%s'\n", line.c_str());
        if (line.startsWith("CATFISH_RELAY|")) {
          String found = line.substring(14);
          int nl = found.indexOf('\n');
          if (nl >= 0) found = found.substring(0, nl);
          found.trim();
          if (validIpString(found)) {
            s_ip = found;
            prefs::setRelayIp(found);
            r.ok = true;
            r.ip = found;
            udp.stop();
            Serial.printf("[NET] relay discovered via UDP at %s\n", found.c_str());
            return r;
          }
        }
      }
      delay(10);
    }
    udp.stop();
  }

  // (4) fail
  r.error = "Relay not found";
  Serial.println("[NET] relay discovery failed");
  return r;
}

Result feed(uint8_t seconds) {
  Result r;

  if (!net_wifi::wifi_is_connected()) {
    r.error = "WiFi down";
    return r;
  }

  if (!s_ip.length()) {
    Result d = discover();
    if (!d.ok) {
      r.error = "No relay (" + d.error + ")";
      return r;
    }
  }

  JsonDocument doc;
  doc["seconds"] = (int)seconds;
  doc["token"] = prefs::getRelayToken();
  String body;
  serializeJson(doc, body);

  const String url = "http://" + s_ip + "/feed";
  Serial.printf("[FEED] POST %s (%u bytes)\n", url.c_str(),
                (unsigned)body.length());

  String resp;
  const int code = httpReq(url, true, body, 6000, resp);
  if (code != 200) {
    r.error = "HTTP " + String(code);
    return r;
  }

  JsonDocument rd;
  if (deserializeJson(rd, resp) != DeserializationError::Ok) {
    r.error = "Bad response";
    return r;
  }
  if (!(rd["ok"] | false)) {
    r.error = "Relay refused";
    return r;
  }

  r.ok = true;
  r.ip = s_ip;
  return r;
}

Status fetchStatus() {
  Status st;

  if (!net_wifi::wifi_is_connected()) {
    st.error = "WiFi down";
    return st;
  }
  if (!s_ip.length()) {
    st.error = "No relay IP";
    return st;
  }

  String resp;
  const int code = httpReq("http://" + s_ip + "/status", false, "", 3000, resp);
  if (code != 200) {
    st.error = "HTTP " + String(code);
    return st;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) {
    st.error = "Bad JSON";
    return st;
  }

  st.ok = true;
  st.feeding = (bool)(doc["feeding"] | false);
  st.lastFeedEpoch = (uint32_t)(doc["lastFeedEpoch"] | 0);
  st.version = String((const char *)(doc["version"] | "unknown"));
  st.state = String((const char *)(doc["state"] | ""));
  return st;
}

} // namespace relay_client
