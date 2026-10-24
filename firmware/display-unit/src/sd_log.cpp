/**
 * @file sd_log.cpp
 * @brief Optional microSD feed-history logging (v1.7.0) — implementation.
 *
 * All card access is wrapped in expander-CS select/deselect so other SPI
 * devices can use the bus between writes. File operations are short (one
 * append line) and run from loop() context — the same place NVS writes
 * already happen, never inside the LVGL or worker tasks.
 */
#include "sd_log.h"

#if CATFISH_FEATURE_SD

#include <SPI.h>
#include <SD.h>

#include <chip/esp_expander_base.hpp>

#include "mqtt_payloads.h"

namespace {

// TF-slot wiring (docs/notes/hardware-4.3b.md)
constexpr int SD_PIN_MOSI = 11;
constexpr int SD_PIN_SCK = 12;
constexpr int SD_PIN_MISO = 13;
constexpr int SD_PIN_DUMMY_CS = 16; // toggled by the SD lib; not connected
// (GPIO16 = CAN RX, unused). The CARD's real CS is expander EXIO4.
constexpr uint8_t SD_EXPANDER_CS_PIN = 4; // EXIO4 = TF_CS
constexpr uint32_t SD_SPI_HZ = 20000000;
constexpr const char *LOG_PATH = "/feeds.csv";
constexpr const char *LOG_OLD_PATH = "/feeds.old.csv";
constexpr uint32_t LOG_MAX_BYTES = 2UL * 1024UL * 1024UL; // rotate at 2 MB

sd_log::State s_state = sd_log::State::Disabled;
esp_expander::Base *s_exp = nullptr;
uint32_t s_bytes_since_rotate = 0;

inline bool csSelect() {
  if (!s_exp) return false;
  return s_exp->digitalWrite(SD_EXPANDER_CS_PIN, LOW);
}

inline void csDeselect() {
  if (s_exp) s_exp->digitalWrite(SD_EXPANDER_CS_PIN, HIGH);
}

void writeHeaderIfEmpty() {
  File f = SD.open(LOG_PATH, FILE_READ);
  if (f) {
    const bool empty = f.size() == 0;
    f.close();
    if (!empty) return;
  }
  File w = SD.open(LOG_PATH, FILE_APPEND);
  if (w) {
    w.println("epoch,iso,mask,seconds,ok");
    w.close();
  }
}

void rotateIfDue() {
  if (s_bytes_since_rotate < LOG_MAX_BYTES) return;
  SD.remove(LOG_OLD_PATH);
  if (SD.rename(LOG_PATH, LOG_OLD_PATH)) {
    Serial.println("[SD] log rotated to " + String(LOG_OLD_PATH));
  }
  s_bytes_since_rotate = 0;
  writeHeaderIfEmpty();
}

} // anonymous namespace

namespace sd_log {

bool begin(esp_expander::Base *expander) {
#if CATFISH_FEATURE_SD
  s_exp = expander;
  if (!s_exp) {
    s_state = State::NoCard;
    Serial.println("[SD] WARN: no IO expander — TF card unusable");
    return false;
  }
  s_exp->pinMode(SD_EXPANDER_CS_PIN, OUTPUT);
  if (!csSelect()) {
    s_state = State::NoCard;
    return false;
  }
  SPI.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, -1);
  const bool ok = SD.begin(SD_PIN_DUMMY_CS, SPI, SD_SPI_HZ);
  csDeselect();
  if (!ok) {
    s_state = State::NoCard;
    Serial.println("[SD] no card mounted (slot empty or wiring issue)");
    return false;
  }
  s_state = State::Mounted;
  s_bytes_since_rotate = 0;
  writeHeaderIfEmpty();
  Serial.printf("[SD] mounted: %llu MB total, %llu MB used\n",
                (unsigned long long)(SD.totalBytes() >> 20),
                (unsigned long long)(SD.usedBytes() >> 20));
  return true;
#else
  (void)expander;
  s_state = State::Disabled;
  return false;
#endif
}

State state() { return s_state; }

String summary() {
  switch (s_state) {
    case State::Disabled:
      return String("disabled");
    case State::NoCard:
      return String("no card");
    case State::Mounted: {
      if (!csSelect()) return String("error");
      const uint64_t freeB = SD.totalBytes() - SD.usedBytes();
      csDeselect();
      char buf[24];
      if (freeB >= (1024ULL * 1024ULL * 1024ULL))
        snprintf(buf, sizeof(buf), "%.1f GB free",
                 (double)freeB / (1024.0 * 1024.0 * 1024.0));
      else
        snprintf(buf, sizeof(buf), "%u MB free", (unsigned)(freeB >> 20));
      return String(buf);
    }
  }
  return String("error");
}

void logFeed(uint32_t epoch, uint8_t tankMask, uint8_t seconds, bool ok) {
  if (s_state != State::Mounted) return;
  if (!csSelect()) {
    Serial.println("[SD] WARN: expander CS write failed, row dropped");
    return;
  }
  File f = SD.open(LOG_PATH, FILE_APPEND);
  if (!f) {
    csDeselect();
    Serial.println("[SD] WARN: open failed, row dropped");
    return;
  }
  const int n = f.printf("%lu,%s,0x%02X,%u,%s\n", (unsigned long)epoch,
                         mqtt_payloads::epochToIso8601(epoch).c_str(),
                         (unsigned)tankMask, (unsigned)seconds,
                         ok ? "ok" : "fail");
  f.close();
  csDeselect();
  if (n > 0) s_bytes_since_rotate += (uint32_t)n;
  rotateIfDue();
}

} // namespace sd_log

#endif // CATFISH_FEATURE_SD
