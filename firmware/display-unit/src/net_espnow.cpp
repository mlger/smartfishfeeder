/**
 * @file net_espnow.cpp
 * @brief Optional ESP-NOW receiver for wireless feed nodes (v1.8.0).
 *
 * The recv callback runs in the WiFi task context: it does NO allocation,
 * no prefs reads, no LVGL — just parse + key check + a zero-timeout
 * ui_event_send() (the same cross-task entry the MQTT manager uses).
 * v1.14.0: the callback also collects token-keyed DISCOVER_RSPs into a
 * mux-guarded table while an offline scan window is open; the actual REQ
 * bursts are sent from the net worker task via discoverRelays().
 */
#include "net_espnow.h"

#if CATFISH_FEATURE_ESPNOW

#include <WiFi.h>
#include <esp_now.h>

#include <catfish_rfnode.h>

#include "prefs.h"
#include "ui/ui.h"
#include "net_wifi.h"

namespace {

net_espnow::State s_state = net_espnow::State::WaitingWifi;
bool s_started = false;
uint32_t s_key = 0;
uint32_t s_accepted = 0;
uint32_t s_rejected = 0;

// Small dedup ring: a puck retry often re-broadcasts the same seq.
constexpr size_t SEQ_RING = 8;
// Pre-filled with 0xFFFF (v1.8.2): a {0}-initialized ring permanently
// rejected the very first seq==0 broadcast of any node (a zero slot
// always 'matched' and was never replaced).
uint16_t s_seen[SEQ_RING] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                             0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
uint8_t s_seen_idx = 0;

// ------------------------------------------------------- v1.14.0 discovery
constexpr size_t DISC_MAX = 8; // more than any sane pond; overflow is logged
struct DiscEntry {
  uint8_t mac[6];
  uint8_t mask;
  uint8_t pulseS;
  uint16_t proto;
};
DiscEntry s_disc[DISC_MAX];
size_t s_discCount = 0;
bool s_discOverflow = false;
bool s_discActive = false; // window open (written by worker, read by WiFi task)
portMUX_TYPE s_disc_mux = portMUX_INITIALIZER_UNLOCKED;
bool s_bcastPeer = false;

bool seqSeen(uint16_t seq) {
  for (size_t i = 0; i < SEQ_RING; i++) {
    if (s_seen[i] == seq) return true;
  }
  s_seen[s_seen_idx] = seq;
  s_seen_idx = (uint8_t)((s_seen_idx + 1) % SEQ_RING);
  return false;
}

void rederiveKey() {
  s_key = catfishRfnodeKeyFromToken(prefs::getRelayToken().c_str());
}

/** v1.14.0: register the broadcast peer once (esp_now_send needs a peer). */
void ensureBroadcastPeer() {
  if (s_bcastPeer) return;
  esp_now_peer_info_t peer = {};
  const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  memcpy(peer.peer_addr, bcast, 6);
  peer.channel = 0;        // 0 = current interface channel (pinned while down)
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) == ESP_OK) {
    s_bcastPeer = true;
  } else {
    Serial.println("[ESPNOW] broadcast peer add FAILED (radio not ready?)");
  }
}

void sendDiscoverReq(uint16_t burst) {
  uint8_t pkt[CATFISH_RFNODE_PKT_LEN];
  const size_t n = catfishRfnodeBuildDiscover(pkt, sizeof(pkt), burst, s_key);
  if (!n) return;
  const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const esp_err_t err = esp_now_send(bcast, pkt, n);
  if (err != ESP_OK) {
    Serial.printf("[ESPNOW] discover REQ send err 0x%X (burst %u)\n",
                  (unsigned)err, (unsigned)burst);
  }
}

/**
 * v1.14.0: a token-keyed DISCOVER_RSP arrived (relay answering our offline
 * scan). Store it deduped-by-MAC while the window is open. Runs in the WiFi
 * task — mux-guarded table writes only, no allocation.
 */
void handleDiscoverRsp(const uint8_t *src, const uint8_t *data, int len) {
  uint8_t mask = 0, pulseS = 0;
  uint16_t proto = 0;
  uint32_t key = 0;
  if (!catfishRfnodeParseDiscoverRsp(data, (size_t)len, &mask, &pulseS,
                                     &proto, &key)) {
    s_rejected++;
    return;
  }
  if (key != s_key) {
    s_rejected++;
    return; // wrong token — stay silent (same trust gate as feed datagrams)
  }
  if (!s_discActive) return; // stray RSP outside a scan window — ignore
  portENTER_CRITICAL(&s_disc_mux);
  for (size_t i = 0; i < s_discCount; i++) {
    if (memcmp(s_disc[i].mac, src, 6) == 0) {
      portEXIT_CRITICAL(&s_disc_mux);
      return; // already recorded this relay (the hub bursts 5 REQs)
    }
  }
  if (s_discCount < DISC_MAX) {
    DiscEntry &e = s_disc[s_discCount++];
    memcpy(e.mac, src, 6);
    e.mask = mask;
    e.pulseS = pulseS;
    e.proto = proto;
  } else {
    s_discOverflow = true;
  }
  portEXIT_CRITICAL(&s_disc_mux);
}

void recvCb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  // sender MAC: not filtered for feed datagrams (key is the gate), but it IS
  // the discovery reply address — captured for the scan table below.
  const uint8_t *src = info ? info->src_addr : nullptr;
  uint8_t cmd = 0xFF, mask = 0, seconds = 0;
  uint16_t seq = 0;
  uint32_t key = 0;
  if (catfishRfnodeParse(data, (size_t)len, &cmd, &mask, &seconds, &seq,
                         &key)) {
    if (key != s_key) {
      s_rejected++;
      Serial.printf("[ESPNOW] rejected node packet (bad key, seq %u)\n",
                    (unsigned)seq);
      return;
    }
    if (seqSeen(seq)) {
      s_rejected++;
      return; // duplicate broadcast
    }
    s_accepted++;

    UiEvent ev;
    ev.type = UI_EVT_RF_FEED;
    // v1.10.0: the datagram still carries seconds (wire compat) — convert to
    // the UI's grams unit here using tank 1's calibrated rate.
    ev.grams = catfishSecondsToGrams(seconds, prefs::getTankGps(1));
    ev.tankMask = mask;
    ev.seq = seq; // v1.13.0: echoed to the relay in /feed "seq" (direct-feed dedup)
    ui::ui_event_send(ev); // zero-timeout; drops silently when the queue is full
    Serial.printf("[ESPNOW] node feed request (%u s ~= %u g, mask 0x%X, seq %u)\n",
                  (unsigned)seconds, (unsigned)ev.grams, (unsigned)mask,
                  (unsigned)seq);
    return;
  }
  // v1.14.0: not a feed datagram — an offline-scan DISCOVER_RSP?
  if (src) handleDiscoverRsp(src, data, len);
}

} // anonymous namespace

namespace net_espnow {

State state() { return s_state; }

bool beginOnce() {
  if (s_started) return true;
  rederiveKey();
  // v1.12.1: a failed init is retried instead of latched — beginOnce() now
  // runs as soon as the radio is up (not only when WiFi is connected), and
  // under the QEMU stub the WiFi driver never starts, so a permanent Error
  // latch would be both wrong on hardware race windows and noisy in CI.
  if (esp_now_init() != ESP_OK) {
    static uint32_t s_init_fails = 0;
    s_init_fails++;
    if (s_init_fails == 1 || (s_init_fails % 100) == 0) { // loop ~50 ms → 5 s
      Serial.printf("[ESPNOW] init failed %u time(s), retrying\n",
                    (unsigned)s_init_fails);
    }
    return false;
  }
  esp_now_register_recv_cb(recvCb);
  s_started = true;
  s_state = State::Listening;
  Serial.println("[ESPNOW] listening for feed nodes "
                 "(pins last AP channel while WiFi is down)");
  return true;
}

void onTokenChanged() {
  if (s_started) rederiveKey();
}

int discoverRelays(uint32_t windowMs, net_espnow::EspnowRelay *out,
                   int maxOut) {
  if (!out || maxOut <= 0) return 0;
  // The radio must be up first — the wifi task brings the STA up and pins
  // the channel. Gate here exactly like the loop-side beginOnce() call
  // (main.cpp): under the QEMU stub the wifi task never runs and esp_now_init
  // would fault inside the SDK (LoadProhibited) instead of returning an error.
  if (!net_wifi::radio_started()) return 0;
  // QEMU / radio-not-yet-up: say "nothing found" so the scan completes
  // cleanly (v1.13.1 contract).
  if (!beginOnce()) return 0;
  ensureBroadcastPeer();

  portENTER_CRITICAL(&s_disc_mux);
  s_discCount = 0;
  s_discOverflow = false;
  portEXIT_CRITICAL(&s_disc_mux);
  s_discActive = true;

  const uint32_t start = millis();
  uint32_t nextSend = start;
  uint16_t burst = 0;
  // ~5 REQ bursts across the window (same cadence as the UDP scan rounds).
  while ((uint32_t)(millis() - start) < windowMs) {
    if ((int32_t)(millis() - nextSend) >= 0) {
      sendDiscoverReq(burst++);
      nextSend = millis() + 300;
    }
    delay(30); // yield so the WiFi task can deliver RSPs into the table
  }
  s_discActive = false;

  portENTER_CRITICAL(&s_disc_mux);
  const size_t found = s_discCount;
  int written = 0;
  for (size_t i = 0; i < found && written < maxOut; i++) {
    memcpy(out[written].mac, s_disc[i].mac, 6);
    out[written].tankMask = s_disc[i].mask;
    out[written].pulseS = s_disc[i].pulseS;
    out[written].proto = s_disc[i].proto;
    written++;
  }
  const bool overflow = s_discOverflow;
  portEXIT_CRITICAL(&s_disc_mux);

  if (overflow) {
    Serial.printf("[ESPNOW] discovery: more than %u relays answered, some dropped\n",
                  (unsigned)DISC_MAX);
  }
  for (int i = 0; i < written; i++) {
    Serial.printf("[ESPNOW] discovery: relay %02X:%02X:%02X:%02X:%02X:%02X "
                  "(mask 0x%X, pulse %u s, proto %u)\n",
                  out[i].mac[0], out[i].mac[1], out[i].mac[2],
                  out[i].mac[3], out[i].mac[4], out[i].mac[5],
                  (unsigned)out[i].tankMask, (unsigned)out[i].pulseS,
                  (unsigned)out[i].proto);
  }
  Serial.printf("[ESPNOW] discovery done: %u REQ bursts, %d relay(s) found\n",
                (unsigned)burst, written);
  return written;
}

} // namespace net_espnow

#else // !CATFISH_FEATURE_ESPNOW

namespace net_espnow {
State state() { return State::Disabled; }
bool beginOnce() { return false; }
void onTokenChanged() {}
int discoverRelays(uint32_t, EspnowRelay *, int) { return 0; }
} // namespace net_espnow

#endif // CATFISH_FEATURE_ESPNOW
