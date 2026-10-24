/**
 * @file relay_client.cpp
 * @brief LAN HTTP + mDNS + UDP-broadcast client for the ESP8266 relay units
 *        (multi-tank: one display unit drives up to CATFISH_MAX_TANKS relays).
 *
 * NOT thread-safe by design: all HTTP/discovery calls happen from the net
 * worker task (net_worker.cpp), which serializes every request. The loop
 * task only touches setTankIp()/applyTankTokens()/ip() — small guarded or
 * NVS-backed accessors — never the HTTP paths.
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
#include <catfish_mech.h>
#include "net_wifi.h"
#include "prefs.h"

namespace {

// Per-tank cached IP + transport-failure streak (worker-task owned; the loop
// only reads ip()/writes setTankIp() with String copies, never concurrently
// with an in-flight request because net_worker serializes everything).
String s_ip[relay_client::MAX_TANKS];

// Consecutive transport-level failures against a CACHED IP. Any HTTP status
// code (even 4xx/5xx) means the relay is ALIVE and resets the streak; only
// connect timeouts / refusals count. After TRANSPORT_FAIL_LIMIT misses the
// cache is dropped so the next feed/probe re-discovers instead of hammering
// a dead address until reboot (stale-DHCP self-heal).
constexpr uint8_t TRANSPORT_FAIL_LIMIT = 3;
uint8_t s_transport_fail_streak[relay_client::MAX_TANKS] = {0};

// Protocol-version mismatch is an operator-visible problem: warn once per boot
// (the 60 s probe cadence would otherwise repeat it forever).
bool s_proto_warned = false;

// Effective per-tank tokens, snapshotted by applyTankTokens() (loop ctx) and
// read by the worker under this spinlock (same pattern as the old single
// s_token guard in prefs).
portMUX_TYPE s_tok_mux = portMUX_INITIALIZER_UNLOCKED;
char s_tok[relay_client::MAX_TANKS][CATFISH_TOKEN_LEN] = {{0}, {0}, {0}, {0}};

int tIdx(uint8_t tank1) {
  const int i = (int)tank1 - 1;
  if (i < 0 || i >= relay_client::MAX_TANKS) return -1;
  return i;
}

void noteTransportFailure(uint8_t tank1) {
  const int i = tIdx(tank1);
  if (i < 0 || !s_ip[i].length()) return;
  s_transport_fail_streak[i]++;
  Serial.printf("[NET] tank %u transport failure #%u at %s\n", (unsigned)tank1,
                (unsigned)s_transport_fail_streak[i], s_ip[i].c_str());
  if (s_transport_fail_streak[i] >= TRANSPORT_FAIL_LIMIT) {
    Serial.printf("[NET] tank %u unreachable - clearing cached IP for re-discovery\n",
                  (unsigned)tank1);
    relay_client::clearTankIp(tank1);
  }
}

void noteTransportSuccess(uint8_t tank1) {
  const int i = tIdx(tank1);
  if (i >= 0) s_transport_fail_streak[i] = 0;
}

/** Effective token for a tank: guarded snapshot written by applyTankTokens(). */
String tankToken(uint8_t tank1) {
  const int i = tIdx(tank1);
  if (i < 0) return String("");
  char buf[CATFISH_TOKEN_LEN];
  portENTER_CRITICAL(&s_tok_mux);
  strlcpy(buf, s_tok[i], sizeof(buf));
  portEXIT_CRITICAL(&s_tok_mux);
  return String(buf);
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

bool ipClaimed(const String &candidate, uint8_t exceptTank1, uint8_t tankCount) {
  for (uint8_t t = 1; t <= tankCount && t <= relay_client::MAX_TANKS; t++) {
    if (t == exceptTank1) continue;
    if (relay_client::ip(t).equalsIgnoreCase(candidate)) return true;
  }
  return false;
}

/**
 * One UDP broadcast round collecting every distinct responder IP.
 * Sends "CATFISH_DISCOVER\n" to the global broadcast AND the subnet-directed
 * broadcast (AP isolation workaround), listens @p windowMs, dedupes.
 * @return number of unique relays found (up to maxOut).
 */
int scanUdpRelays(String out[], int maxOut, uint32_t windowMs) {
  if (maxOut <= 0) return 0;
  WiFiUDP udp;
  bool bound = false;
  for (uint16_t port = 5051; port <= 5055 && !bound; port++) {
    bound = udp.begin(port); // local (source) port; relays reply here
  }
  if (!bound) {
    Serial.println("[NET] discovery UDP bind failed (ports 5051-5055)");
    return 0;
  }

  udp.beginPacket(IPAddress(255, 255, 255, 255), CATFISH_DISCOVERY_UDP_PORT);
  udp.print(CATFISH_DISCOVERY_REQ "\n");
  udp.endPacket();
  const uint32_t lip = (uint32_t)WiFi.localIP();
  const uint32_t mask = (uint32_t)WiFi.subnetMask();
  const IPAddress subnetBc((lip & mask) | (~mask));
  if (subnetBc != IPAddress(255, 255, 255, 255)) {
    udp.beginPacket(subnetBc, CATFISH_DISCOVERY_UDP_PORT);
    udp.print(CATFISH_DISCOVERY_REQ "\n");
    udp.endPacket();
  }

  int found = 0;
  const uint32_t t0 = millis();
  while (millis() - t0 < windowMs) {
    int p = udp.parsePacket();
    if (p > 0) {
      char buf[80] = {0};
      udp.read(buf, sizeof(buf) - 1);
      String line(buf);
      line.trim();
      if (line.startsWith(CATFISH_DISCOVERY_RSP_PREFIX)) {
        String ip = line.substring(strlen(CATFISH_DISCOVERY_RSP_PREFIX));
        int nl = ip.indexOf('\n');
        if (nl >= 0) ip = ip.substring(0, nl);
        ip.trim();
        if (validIpString(ip)) {
          bool dup = false;
          for (int i = 0; i < found; i++) {
            if (out[i].equalsIgnoreCase(ip)) { dup = true; break; }
          }
          if (!dup && found < maxOut) {
            out[found++] = ip;
            Serial.printf("[NET] discovery reply: relay at %s\n", ip.c_str());
          }
        }
      }
    }
    delay(10);
  }
  udp.stop();
  return found;
}

} // anonymous namespace

namespace relay_client {

void init() {
  for (uint8_t t = 1; t <= MAX_TANKS; t++) {
    const int i = tIdx(t);
    s_ip[i] = prefs::getTankIp(t);
    s_transport_fail_streak[i] = 0;
    if (s_ip[i].length()) {
      Serial.printf("[NET] tank %u: saved IP %s (will validate on first use)\n",
                    (unsigned)t, s_ip[i].c_str());
    }
  }
  applyTankTokens();
}

void applyTankTokens() {
  portENTER_CRITICAL(&s_tok_mux);
  for (uint8_t t = 1; t <= MAX_TANKS; t++) {
    const String tok = prefs::effectiveTankToken(t);
    strlcpy(s_tok[tIdx(t)], tok.c_str(), sizeof(s_tok[tIdx(t)]));
  }
  portEXIT_CRITICAL(&s_tok_mux);
}

bool hasIp(uint8_t tank1) {
  const int i = tIdx(tank1);
  return i >= 0 && s_ip[i].length() > 0;
}

String ip(uint8_t tank1) {
  const int i = tIdx(tank1);
  return (i >= 0) ? s_ip[i] : String("");
}

void setTankIp(uint8_t tank1, const String &ipNew) {
  const int i = tIdx(tank1);
  if (i < 0) return;
  s_ip[i] = ipNew;
  prefs::setTankIp(tank1, ipNew); // NVS write; called from loop or worker
  if (ipNew.length()) noteTransportSuccess(tank1);
}

void clearTankIp(uint8_t tank1) {
  const int i = tIdx(tank1);
  if (i < 0) return;
  s_ip[i] = "";
  s_transport_fail_streak[i] = 0;
  prefs::setTankIp(tank1, "");
}

Result discoverTank(uint8_t tank1) {
  Result r;
  const int i = tIdx(tank1);
  if (i < 0) {
    r.error = "Bad tank id";
    return r;
  }

  if (!net_wifi::wifi_is_connected()) {
    r.error = "WiFi down";
    return r;
  }

  // (1) Saved IP validated by GET /health (2 s timeout)
  if (s_ip[i].length()) {
    if (healthCheck(s_ip[i], 2000)) {
      noteTransportSuccess(tank1);
      r.ok = true;
      r.ip = s_ip[i];
      Serial.printf("[NET] tank %u relay healthy at saved IP %s\n",
                    (unsigned)tank1, s_ip[i].c_str());
      return r;
    }
    Serial.printf("[NET] tank %u saved relay IP %s not responding\n",
                  (unsigned)tank1, s_ip[i].c_str());
  }

  // (2) One broadcast round, collect ALL responders, then bind the first one
  //     not already claimed by another configured tank.
  String cands[CATFISH_MAX_TANKS];
  int n = scanUdpRelays(cands, CATFISH_MAX_TANKS, 1500);

  // mDNS is an extra candidate; with multiple relays sharing the
  // "catfish-relay" hostname it is ambiguous, but harmless when it agrees
  // with the UDP answers (deduped via ipClaimed logic below).
  IPAddress resolved = MDNS.queryHost(CATFISH_RELAY_HOSTNAME, 1000);
  if (resolved != IPAddress(0, 0, 0, 0)) {
    String mIp = resolved.toString();
    if (validIpString(mIp) && n < CATFISH_MAX_TANKS) {
      bool dup = false;
      for (int k = 0; k < n; k++) dup |= cands[k].equalsIgnoreCase(mIp);
      if (!dup) cands[n++] = mIp;
    }
  }

  const uint8_t tankCount = prefs::getTankCount();
  for (int k = 0; k < n; k++) {
    if (ipClaimed(cands[k], tank1, tankCount)) continue;
    setTankIp(tank1, cands[k]);
    r.ok = true;
    r.ip = cands[k];
    Serial.printf("[NET] tank %u relay discovered at %s\n", (unsigned)tank1,
                  cands[k].c_str());
    return r;
  }

  // (3) fail
  r.error = n ? "No free relay found" : "Relay not found";
  Serial.printf("[NET] tank %u discovery failed (%s)\n", (unsigned)tank1,
                r.error.c_str());
  return r;
}

Result feedTank(uint8_t tank1, uint8_t seconds) {
  Result r;
  const int i = tIdx(tank1);
  if (i < 0) {
    r.error = "Bad tank id";
    return r;
  }

  if (!net_wifi::wifi_is_connected()) {
    r.error = "WiFi down";
    return r;
  }

  if (!s_ip[i].length()) {
    Result d = discoverTank(tank1);
    if (!d.ok) {
      r.error = "No relay (" + d.error + ")";
      return r;
    }
  }

  JsonDocument doc;
  doc["seconds"] = (int)seconds;
  doc["token"] = tankToken(tank1);
  String body;
  serializeJson(doc, body);

  const String url0 = "http://" + s_ip[i] + CATFISH_EP_FEED;
  Serial.printf("[FEED] tank %u POST %s (%u bytes)\n", (unsigned)tank1,
                url0.c_str(), (unsigned)body.length());

  String resp;
  int code = httpReq(url0, true, body, 6000, resp);
  if (code > 0) noteTransportSuccess(tank1);

  if (code <= 0) {
    // Transport failure: the cached IP may be stale (DHCP moved the relay).
    // Self-heal: drop the cache, re-discover, retry ONCE against a new IP.
    noteTransportFailure(tank1);
    const String stale = s_ip[i];
    if (stale.length()) {
      Serial.printf("[FEED] no response from tank %u at %s - re-discovering\n",
                    (unsigned)tank1, stale.c_str());
      clearTankIp(tank1);
      const Result d = discoverTank(tank1);
      if (!d.ok) {
        r.error = "No relay (" + d.error + ")";
        return r;
      }
      if (d.ip != stale) {
        doc["token"] = tankToken(tank1); // token unchanged, rebuild anyway
        body.clear();
        serializeJson(doc, body);
        code = httpReq("http://" + s_ip[i] + CATFISH_EP_FEED, true, body, 6000,
                       resp);
        if (code > 0) noteTransportSuccess(tank1);
        if (code <= 0) {
          noteTransportFailure(tank1);
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
  r.ip = s_ip[i];
  // What the relay actually applied (it clamps to maxPulseS and reports a note)
  r.seconds_applied = (uint8_t)(rd["seconds"] | (int)seconds);
  r.clamped = rd["note"].is<const char *>();
  if (r.clamped) r.note = String((const char *)(rd["note"] | ""));
  return r;
}

Status fetchStatusTank(uint8_t tank1) {
  Status st;
  const int i = tIdx(tank1);
  if (i < 0) {
    st.error = "Bad tank id";
    return st;
  }

  if (!net_wifi::wifi_is_connected()) {
    st.error = "WiFi down";
    return st;
  }
  if (!s_ip[i].length()) {
    st.error = "No relay IP";
    return st;
  }

  String resp;
  const int code =
      httpReq("http://" + s_ip[i] + CATFISH_EP_STATUS, false, "", 3000, resp);
  if (code <= 0) {
    noteTransportFailure(tank1);
    st.error = describeError(code, resp);
    return st;
  }
  noteTransportSuccess(tank1);
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
  st.protocolVersion = (int)(doc["proto"] | -1);
  st.mechanism = String((const char *)(doc["mechanism"] | "")); // "" = pre-P4 relay
  st.servoRest = (int)(doc["servoRestDeg"] | -1);
  st.servoFeed = (int)(doc["servoFeedDeg"] | -1);
  st.stepHz = (int)(doc["stepHz"] | -1);

  if (st.protocolVersion >= 0 && st.protocolVersion != CATFISH_PROTOCOL_VERSION &&
      !s_proto_warned) {
    s_proto_warned = true;
    Serial.printf(
        "[NET] WARNING: relay protocol v%d but display expects v%d - flash BOTH "
        "units to matching firmware versions\n",
        st.protocolVersion, CATFISH_PROTOCOL_VERSION);
  }
  return st;
}

Result configureMechTank(uint8_t tank1, uint8_t mech, uint8_t restDeg,
                         uint8_t feedDeg, uint16_t stepHz, uint8_t stepDir) {
  Result r;
  const int i = tIdx(tank1);
  if (i < 0) {
    r.error = "Bad tank id";
    return r;
  }

  if (!net_wifi::wifi_is_connected()) {
    r.error = "WiFi down";
    return r;
  }
  if (mech >= CATFISH_MECH_COUNT) {
    r.error = "Bad mechanism";
    return r;
  }
  if (!s_ip[i].length()) {
    Result d = discoverTank(tank1);
    if (!d.ok) {
      r.error = "No relay (" + d.error + ")";
      return r;
    }
  }

  // Pre-clamp here too: the relay clamps again (shared catfish_mech.h helpers).
  JsonDocument doc;
  doc["token"] = tankToken(tank1);
  doc["mechanism"] = catfishMechName(mech);
  doc["servoRestDeg"] = (int)catfishClampServoDeg(restDeg);
  doc["servoFeedDeg"] = (int)catfishClampServoDeg(feedDeg);
  doc["stepHz"] = (int)catfishClampStepHz(stepHz);
  doc["stepDir"] = (int)catfishClampStepDir(stepDir);
  String body;
  serializeJson(doc, body);

  String resp;
  const int code =
      httpReq("http://" + s_ip[i] + CATFISH_EP_CONFIG, true, body, 4000, resp);
  if (code > 0) noteTransportSuccess(tank1);
  if (code <= 0) {
    noteTransportFailure(tank1);
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
  r.ip = s_ip[i];
  return r;
}

Result rotateTokenTank(uint8_t tank1, const String &newToken) {
  Result r;
  const int i = tIdx(tank1);
  if (i < 0) {
    r.error = "Bad tank id";
    return r;
  }

  if (!net_wifi::wifi_is_connected()) {
    r.error = "WiFi down";
    return r;
  }
  if (!newToken.length() || newToken.length() >= CATFISH_TOKEN_LEN) {
    r.error = "Bad token length";
    return r;
  }
  if (!s_ip[i].length()) {
    Result d = discoverTank(tank1);
    if (!d.ok) {
      r.error = "No relay (" + d.error + ")";
      return r;
    }
  }

  JsonDocument doc;
  doc["token"] = tankToken(tank1); // current credential
  doc["newToken"] = newToken;
  String body;
  serializeJson(doc, body);

  String resp;
  const int code =
      httpReq("http://" + s_ip[i] + CATFISH_EP_CONFIG, true, body, 4000, resp);
  if (code > 0) noteTransportSuccess(tank1);
  if (code <= 0) {
    noteTransportFailure(tank1);
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
  r.ip = s_ip[i];
  return r;
}

int scanAndBindTanks(uint8_t tankCount, uint8_t *assignedMask) {
  if (!net_wifi::wifi_is_connected()) return -1;
  if (assignedMask) *assignedMask = 0;
  if (tankCount < 1) tankCount = 1;
  if (tankCount > MAX_TANKS) tankCount = MAX_TANKS;

  String cands[CATFISH_MAX_TANKS];
  const int n = scanUdpRelays(cands, CATFISH_MAX_TANKS, 1500);
  Serial.printf("[NET] scan: %d relay(s) answered\n", n);
  if (!n) return 0;

  int live = 0;
  for (uint8_t t = 1; t <= tankCount; t++) {
    // keep a tank whose saved IP is still alive
    if (hasIp(t) && healthCheck(ip(t), 2000)) {
      noteTransportSuccess(t);
      live++;
      continue;
    }
    // bind the first candidate not claimed by another tank
    const char *boundTo = nullptr;
    for (int k = 0; k < n; k++) {
      if (ipClaimed(cands[k], t, tankCount)) continue;
      setTankIp(t, cands[k]);
      if (assignedMask) *assignedMask |= (1u << (t - 1));
      live++;
      Serial.printf("[NET] scan: tank %u bound to %s\n", (unsigned)t,
                    cands[k].c_str());
      boundTo = cands[k].c_str();
      break;
    }
    (void)boundTo;
  }
  return live;
}

} // namespace relay_client
