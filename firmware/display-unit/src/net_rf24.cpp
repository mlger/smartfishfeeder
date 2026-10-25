/**
 * @file net_rf24.cpp
 * @brief Optional nRF24L01+ receiver for wireless feed nodes (v1.8.0).
 *
 * "rf-task" polls the radio every 50 ms under the shared SPI bus mutex,
 * parses the Catfish node datagram and routes accepted requests into the
 * shared UiEvent queue (same trust + dedup model as net_espnow).
 */
#include "net_rf24.h"

#if CATFISH_FEATURE_NRF24

#include <RF24.h>
#include <SPI.h>

#include <catfish_rfnode.h>

#include "prefs.h"
#include "spi_bus_guard.h"
#include "ui/ui.h"

namespace {

constexpr uint8_t NODE_ADDR[5] = {0xCA, 0x74, 0xF1, 0x5B, 0x01};

RF24 s_radio(CATFISH_RF24_CE, CATFISH_RF24_CSN);
net_rf24::State s_state = net_rf24::State::Error;
uint32_t s_key = 0;
uint32_t s_accepted = 0;
uint32_t s_rejected = 0;

constexpr size_t SEQ_RING = 8;
// Pre-filled with 0xFFFF (v1.8.2): a {0}-initialized ring permanently
// rejected the very first seq==0 broadcast of any node (a zero slot
// always 'matched' and was never replaced).
uint16_t s_seen[SEQ_RING] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                             0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
uint8_t s_seen_idx = 0;

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

void routePacket(const uint8_t *buf, size_t len) {
  uint8_t cmd = 0xFF, mask = 0, seconds = 0;
  uint16_t seq = 0;
  uint32_t key = 0;
  if (!catfishRfnodeParse(buf, len, &cmd, &mask, &seconds, &seq, &key) ||
      key != s_key) {
    s_rejected++;
    return;
  }
  if (seqSeen(seq)) {
    s_rejected++;
    return;
  }
  s_accepted++;

  UiEvent ev;
  ev.type = UI_EVT_RF_FEED;
  ev.a = seconds; // 0 = hub default pulse
  ev.tankMask = mask;
  ui::ui_event_send(ev);
  Serial.printf("[RF24] node feed request (%u s, mask 0x%X, seq %u)\n",
                (unsigned)seconds, (unsigned)mask, (unsigned)seq);
}

void rfTask(void *) {
  uint8_t buf[CATFISH_RFNODE_PKT_LEN];
  for (;;) {
    bool got = false;
    if (spi_bus_guard::tryTake()) {
      if (s_state == net_rf24::State::Listening && s_radio.available()) {
        s_radio.read(buf, sizeof(buf));
        got = true;
      }
      spi_bus_guard::give();
    }
    if (got) routePacket(buf, sizeof(buf));
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

} // anonymous namespace

namespace net_rf24 {

State state() { return s_state; }

bool begin() {
  rederiveKey();
  spi_bus_guard::take();
  // Pins may already be up from sd_log (shared bus); SPI.begin is idempotent
  // enough here (same pins, same controller).
  SPI.begin(12, 13, 11, -1);
  const bool ok = s_radio.begin() && s_radio.isChipConnected();
  if (ok) {
    s_radio.setDataRate(RF24_1MBPS);
    s_radio.setChannel(CATFISH_RF24_CHANNEL);
    // v1.8.2: static 12-byte payload. Without this the radio uses the 32-byte
    // default, and reading 12 bytes per packet left FIFO residue that
    // desynchronized every subsequent read.
    s_radio.setPayloadSize(CATFISH_RFNODE_PKT_LEN);
    s_radio.openReadingPipe(1, NODE_ADDR);
    s_radio.startListening();
  }
  spi_bus_guard::give();
  if (!ok) {
    s_state = State::Error;
    Serial.println("[RF24] WARN: nRF24L01 not found on CE " +
                   String(CATFISH_RF24_CE) + "/CSN " + String(CATFISH_RF24_CSN));
    return false;
  }
  s_state = State::Listening;
  if (xTaskCreatePinnedToCore(rfTask, "rf-task", 4096, nullptr, 1, nullptr,
                              0) != pdPASS) {
    s_state = State::Error;
    Serial.println("[RF24] WARN: rf-task spawn failed");
    return false;
  }
  Serial.printf("[RF24] listening: ch %u, 1 Mbps, CE %d/CSN %d\n",
                (unsigned)CATFISH_RF24_CHANNEL, CATFISH_RF24_CE,
                CATFISH_RF24_CSN);
  return true;
}

void onTokenChanged() {
  if (s_state == net_rf24::State::Listening) rederiveKey();
}

} // namespace net_rf24

#else // !CATFISH_FEATURE_NRF24

namespace net_rf24 {
State state() { return State::Disabled; }
bool begin() { return false; }
void onTokenChanged() {}
} // namespace net_rf24

#endif // CATFISH_FEATURE_NRF24
