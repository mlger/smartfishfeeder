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
#include "spi_bus_guard.h"

namespace {

// TF-slot wiring (docs/notes/hardware-4.3b.md)
constexpr int SD_PIN_MOSI = 11;
constexpr int SD_PIN_SCK = 12;
constexpr int SD_PIN_MISO = 13;
constexpr int SD_PIN_DUMMY_CS = 16; // toggled by the SD lib; not connected
// (GPIO16 = CAN RX, unused). The CARD's real CS is expander EXIO4.
constexpr uint8_t SD_EXPANDER_CS_PIN = 4; // EXIO4 = TF_CS
constexpr const char *LOG_PATH = "/feeds.csv";
constexpr const char *LOG_OLD_PATH = "/feeds.old.csv";
constexpr uint32_t LOG_MAX_BYTES = 2UL * 1024UL * 1024UL; // rotate at 2 MB

sd_log::State s_state = sd_log::State::Disabled;
esp_expander::Base *s_exp = nullptr;
uint32_t s_bytes_since_rotate = 0;
uint8_t s_remount_slot = 0; // round-robin over the speed table (remount())

// Mount speeds tried in order. Many cards are fine at 20 MHz; marginal ones
// (long ribbon, slower/older cards, some SDHC/SDXC brands) only train at
// 10 MHz or 4 MHz — a single fixed speed turned into "no sd card" reports.
constexpr uint32_t SD_SPEEDS_MHZ[] = {20, 10, 4};
constexpr size_t SD_SPEED_COUNT = sizeof(SD_SPEEDS_MHZ) / sizeof(SD_SPEEDS_MHZ[0]);

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

/**
 * One full mount attempt at a given SPI clock. Returns true and leaves the
 * card mounted when successful. spi_bus_guard is taken for the duration.
 */
static bool mountAttempt(uint32_t hz) {
  spi_bus_guard::take();
  s_exp->pinMode(SD_EXPANDER_CS_PIN, OUTPUT);
  if (!csSelect()) {
    spi_bus_guard::give();
    Serial.println("[SD] WARN: expander CS write failed");
    return false;
  }
  SPI.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, -1);
  const bool ok = SD.begin(SD_PIN_DUMMY_CS, SPI, hz);
  csDeselect();
  if (!ok) {
    spi_bus_guard::give();
    return false;
  }
  s_state = State::Mounted;
  // Seed the rotation counter with the existing log size (v1.8.2 fix).
  {
    csSelect();
    File f = SD.open(LOG_PATH, FILE_READ);
    if (f) {
      s_bytes_since_rotate = (uint32_t)f.size();
      f.close();
    }
    csDeselect();
  }
  writeHeaderIfEmpty();
  const uint64_t tot = SD.totalBytes(), used = SD.usedBytes();
  spi_bus_guard::give();
  Serial.printf("[SD] mounted at %u MHz: %llu MB total, %llu MB used\n",
                (unsigned)(hz / 1000000), (unsigned long long)(tot >> 20),
                (unsigned long long)(used >> 20));
  return true;
}

bool begin(esp_expander::Base *expander) {
#if CATFISH_FEATURE_SD
  s_exp = expander;
  if (!s_exp) {
    s_state = State::NoCard;
    Serial.println("[SD] WARN: no IO expander — TF card unusable");
    return false;
  }
  for (size_t i = 0; i < SD_SPEED_COUNT; i++) {
    if (mountAttempt(SD_SPEEDS_MHZ[i] * 1000000UL)) {
      return true;
    }
  }
  s_state = State::NoCard;
  Serial.println("[SD] no card mounted (slot empty, card not FAT-formatted, "
                 "or wiring issue) — will recheck every 10 s");
  return false;
#else
  (void)expander;
  s_state = State::Disabled;
  return false;
#endif
}

/**
 * v1.8.4: hot-plug recheck for the boot-time "no card" case (card inserted
 * after boot, slow-to-power card, or a marginal card that needs a slower
 * clock). Tries ONE speed per call (round-robin) so a cardless slot costs
 * at most ~0.5 s of loop() time per recheck instead of stalling all three.
 * Returns true only on a successful (re)mount.
 */
bool remount() {
#if CATFISH_FEATURE_SD
  if (s_state == State::Mounted || !s_exp) return false;
  const uint32_t hz = SD_SPEEDS_MHZ[s_remount_slot % SD_SPEED_COUNT] * 1000000UL;
  s_remount_slot++;
  return mountAttempt(hz);
#else
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
      spi_bus_guard::take();
      if (!csSelect()) {
        spi_bus_guard::give();
        return String("error");
      }
      const uint64_t freeB = SD.totalBytes() - SD.usedBytes();
      csDeselect();
      spi_bus_guard::give();
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
  spi_bus_guard::take();
  if (!csSelect()) {
    spi_bus_guard::give();
    Serial.println("[SD] WARN: expander CS write failed, row dropped");
    return;
  }
  File f = SD.open(LOG_PATH, FILE_APPEND);
  if (!f) {
    csDeselect();
    spi_bus_guard::give();
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
  spi_bus_guard::give();
}

} // namespace sd_log

#endif // CATFISH_FEATURE_SD
