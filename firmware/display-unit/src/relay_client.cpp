/**
 * @file relay_client.cpp
 * @brief LAN HTTP + mDNS + UDP-broadcast client for the ESP8266 relay unit.
 *
 * NOT thread-safe by design: all calls happen from the net worker task
 * (net_worker.cpp), which serializes every request.
 */
#include "relay_client.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <catfish_protocol.h>
#include "net_wifi.h"
#include "prefs.h"

namespace {

String s_ip; // cached relay IP, dotted string ("" = unknown)

// Consecutive transport-level failures against the CACHED IP. Any HTTP status
// code (even 4xx/5xx) means the relay is ALIVE and resets the streak; only
// connect timeouts / refusals count. After TRANSPORT_FAIL_LIMIT misses the
// cache is dropped so the next feed/probe re-discovers instead of hammering
// a dead address until reboot (stale-DHCP self-heal).
constexpr uint8_t TRANSPORT_FAIL_LIMIT = 3;
uint8_t s_transport_fail_streak = 0;

void clearIpCache() {
  s_ip = "";
  prefs::setRelayIp("");
  s_transport_fail_streak = 0;
}

void noteTransportFailure() {
  if (!s_ip.length()) return;
  s_transport_fail_streak++;
  Serial.printf("[NET] relay transport failure #%u at %s\n",
                (unsigned)s_transport_fail_streak, s_ip.c_str());
  if (s_transport_fail_streak >= TRANSPORT_FAIL_LIMIT) {
    Serial.println("[NET] relay unreachable - clearing cached IP for re-discovery");
    clearIpCache();
  }
}

void noteTransportSuccess() { s_transport_fail_streak = 0; }

/** Build the /feed POST body (token read fresh from prefs each attempt). */
String feedBody(uint8_t seconds) {
  JsonDocument doc;
  doc["seconds"] = (int)seconds;
  doc["token"] = prefs::getRelayToken();
  String body;
  serializeJson(doc, body);
  return body;
}

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
  int code = httpReq("http://" + ip + CATFISH_EP_HEALTH, false, "", timeout_ms, resp);
  return (code >= 200 && code < 300);
}

/** Map an error HTTP response body ({"ok":false,"error":"..."}) to friendly text. */
String describeError(int code, const String &body) {
  String err;
  JsonDocument doc;
  if (deserializeJson(doc, body) == DeserializationError::Ok) {
    err = String((const char *)(doc["error"] | ""));
  }
  if (err == "auth") return "Token mismatch";
  if (err == "busy") return "Relay busy";
  if (err == "bad_seconds") return "Invalid duration";
  if (err == "bad_json") return "Bad request";
  if (err.length()) return err; // relay-defined error string, pass through
  // No parsable body: fall back to the status code.
  if (code == 401 || code == 403) return "Token mismatch";
  if (code == 409) return "Relay busy";
  if (code < 0) return "No response";
  return "HTTP " + String(code);
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
      noteTransportSuccess();
      r.ok = true;
      r.ip = s_ip;
      Serial.printf("[NET] relay healthy at saved IP %s\n", s_ip.c_str());
      return r;
    }
    Serial.printf("[NET] saved relay IP %s not responding\n", s_ip.c_str());
  }

  // (2) mDNS query for the relay hostname (we advertise as catfishfeeder)
  IPAddress resolved = MDNS.queryHost(CATFISH_RELAY_HOSTNAME, 2000);
  if (resolved != IPAddress(0, 0, 0, 0)) {
    String mIp = resolved.toString();
    if (validIpString(mIp)) {
      Serial.printf("[NET] mDNS resolved %s -> %s\n", CATFISH_RELAY_HOSTNAME,
                    mIp.c_str());
      s_ip = mIp;
      prefs::setRelayIp(mIp);
      noteTransportSuccess();
      r.ok = true;
      r.ip = mIp;
      return r;
    }
  }

  // (3) UDP broadcast discovery. Some APs filter 255.255.255.255, so we send
  // to both the global and the subnet-directed broadcast address.
  const uint32_t lip = (uint32_t)WiFi.localIP();
  const uint32_t mask = (uint32_t)WiFi.subnetMask();
  const IPAddress subnetBc((lip & mask) | (~mask));
  WiFiUDP udp;
  bool bound = false;
  for (uint16_t port = 5051; port <= 5055 && !bound; port++) {
    bound = udp.begin(port); // local (source) port; relay replies here
  }
  if (bound) {
    udp.beginPacket(IPAddress(255, 255, 255, 255), CATFISH_DISCOVERY_UDP_PORT);
    udp.print(CATFISH_DISCOVERY_REQ "\n");
    udp.endPacket();
    if (subnetBc != IPAddress(255, 255, 255, 255)) {
      udp.beginPacket(subnetBc, CATFISH_DISCOVERY_UDP_PORT);
      udp.print(CATFISH_DISCOVERY_REQ "\n");
      udp.endPacket();
    }

    const uint32_t t0 = millis();
    while (millis() - t0 < 1500) {
      int p = udp.parsePacket();
      if (p > 0) {
        char buf[80] = {0};
        udp.read(buf, sizeof(buf) - 1);
        String line(buf);
        line.trim();
        Serial.printf("[NET] discovery reply: '%s'\n", line.c_str());
        if (line.startsWith(CATFISH_DISCOVERY_RSP_PREFIX)) {
          String found = line.substring(strlen(CATFISH_DISCOVERY_RSP_PREFIX));
          int nl = found.indexOf('\n');
          if (nl >= 0) found = found.substring(0, nl);
          found.trim();
          if (validIpString(found)) {
            s_ip = found;
            prefs::setRelayIp(found);
            noteTransportSuccess();
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
  } else {
    Serial.println("[NET] discovery UDP bind failed (ports 5051-5055)");
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

  const String url0 = "http://" + s_ip + CATFISH_EP_FEED;
  String body = feedBody(seconds);
  Serial.printf("[FEED] POST %s (%u bytes)\n", url0.c_str(),
                (unsigned)body.length());

  String resp;
  int code = httpReq(url0, true, body, 6000, resp);
  if (code > 0) noteTransportSuccess();

  if (code <= 0) {
    // Transport failure: the cached IP may be stale (DHCP moved the relay).
    // Self-heal: drop the cache, re-discover, retry ONCE against a new IP.
    noteTransportFailure();
    const String stale = s_ip;
    if (stale.length()) {
      Serial.printf("[FEED] no response from %s - re-discovering\n",
                    stale.c_str());
      clearIpCache();
      const Result d = discover();
      if (!d.ok) {
        r.error = "No relay (" + d.error + ")";
        return r;
      }
      if (d.ip != stale) {
        body = feedBody(seconds);
        code = httpReq("http://" + s_ip + CATFISH_EP_FEED, true, body, 6000,
                       resp);
        if (code > 0) noteTransportSuccess();
        if (code <= 0) {
          noteTransportFailure();
          r.error = "No response";
          return r;
        }
      }
      // Same IP re-confirmed by discovery: fall through and report the
      // original transport error.
    } else {
      r.error = "No response";
      return r;
    }
  }

  if (code != 200) {
    r.error = describeError(code, resp);
    return r;
  }

  JsonDocument rd;
  if (deserializeJson(rd, resp) != DeserializationError::Ok) {
    r.error = "Bad response";
    return r;
  }
  if (!(rd["ok"] | false)) {
    r.error = describeError(code, resp);
    return r;
  }

  r.ok = true;
  r.ip = s_ip;
  // What the relay actually applied (it clamps to maxPulseS and reports a note)
  r.seconds_applied = (uint8_t)(rd["seconds"] | (int)seconds);
  r.clamped = rd["note"].is<const char*>();
  if (r.clamped) r.note = String((const char *)(rd["note"] | ""));
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
  const int code =
      httpReq("http://" + s_ip + CATFISH_EP_STATUS, false, "", 3000, resp);
  if (code <= 0) {
    noteTransportFailure();
    st.error = describeError(code, resp);
    return st;
  }
  noteTransportSuccess();
  if (code != 200) {
    st.error = describeError(code, resp);
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

Result rotateToken(const String &newToken) {
  Result r;

  if (!net_wifi::wifi_is_connected()) {
    r.error = "WiFi down";
    return r;
  }
  if (!newToken.length() || newToken.length() >= CATFISH_TOKEN_LEN) {
    r.error = "Bad token length";
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
  doc["token"] = prefs::getRelayToken(); // current credential
  doc["newToken"] = newToken;
  String body;
  serializeJson(doc, body);

  String resp;
  const int code =
      httpReq("http://" + s_ip + CATFISH_EP_CONFIG, true, body, 4000, resp);
  if (code > 0) noteTransportSuccess();
  if (code <= 0) {
    noteTransportFailure();
    r.error = describeError(code, resp);
    return r;
  }
  if (code != 200) {
    r.error = describeError(code, resp);
    return r;
  }

  JsonDocument rd;
  if (deserializeJson(rd, resp) != DeserializationError::Ok ||
      !(rd["ok"] | false)) {
    r.error = describeError(code, resp);
    return r;
  }

  r.ok = true;
  r.ip = s_ip;
  return r;
}

} // namespace relay_client
