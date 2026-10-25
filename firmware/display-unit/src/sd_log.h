/**
 * @file sd_log.h
 * @brief Optional microSD feed-history logging (v1.7.0).
 *
 * The 4.3B board's TF slot is wired to SPI (MOSI GPIO11, SCK GPIO12,
 * MISO GPIO13) with the card's CS on the CH422G expander (EXIO4). The
 * expander CS is asserted ONLY around each SD operation, so the bus is free
 * for other devices (e.g. the v1.8.0 nRF24L01 on the same SPI pins) between
 * writes.
 *
 * Feature gate: compiled in by default (`-DCATFISH_FEATURE_SD=1` in
 * platformio.ini). Remove the flag (or set 0) to build without SD support —
 * every call becomes a no-op and the UI reports "disabled". A missing card
 * at boot degrades gracefully to "no card".
 *
 * Log format: /feeds.csv — `epoch,iso8601,tankMask,seconds,ok`
 * Rotated to /feeds.old.csv when it passes CATFISH_SD_LOG_MAX_BYTES.
 */
#ifndef CATFISH_SD_LOG_H
#define CATFISH_SD_LOG_H

#include <Arduino.h>
#include <stdint.h>

#ifndef CATFISH_FEATURE_SD
#define CATFISH_FEATURE_SD 1
#endif

namespace esp_expander {
class Base;
}

namespace sd_log {

enum State : uint8_t {
  Disabled = 0, // compiled out (CATFISH_FEATURE_SD = 0)
  NoCard,       // mount failed at boot (no card / wiring)
  Mounted,
};

/** Mount the card (blocking ~100-500 ms across speed retries — setup() only). */
bool begin(esp_expander::Base *expander);

/**
 * v1.8.4: hot-plug recheck. Call from loop() while state() == NoCard (the
 * caller owns the cadence, ~every 10 s). Tries ONE mount speed per call
 * (round-robin 20/10/4 MHz) so a cardless slot costs ≤ ~0.5 s of loop time
 * per recheck. Returns true only on a successful mount (caller should
 * refresh the SD label / toast). No-op when Mounted, Disabled or no expander.
 */
bool remount();

/** Current state (cheap; safe anywhere). */
State state();

/**
 * One-line human summary for the Settings card: "disabled", "no card",
 * "1.9 GB free" or "error". Does SD I/O — loop() context only.
 */
String summary();

/**
 * Append one feed event. epoch 0 = clock not valid at the time. Called from
 * loop() context after a worker feed result; failures only log to Serial.
 */
void logFeed(uint32_t epoch, uint8_t tankMask, uint8_t seconds, bool ok);

} // namespace sd_log

#endif // CATFISH_SD_LOG_H
