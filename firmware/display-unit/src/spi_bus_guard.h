/**
 * @file spi_bus_guard.h
 * @brief Mutex for the (potentially shared) display-unit SPI bus.
 *
 * The TF card (sd_log) and the optional nRF24L01 (net_rf24) can sit on the
 * same SPI pins (SCK 12 / MISO 13 / MOSI 11). Both modules route ALL their
 * SPI traffic through this mutex, so the two features coexist safely even
 * from different tasks (SD: loop task; nRF24: "rf-task").
 */
#ifndef CATFISH_SPI_BUS_GUARD_H
#define CATFISH_SPI_BUS_GUARD_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace spi_bus_guard {

/** Process-wide SPI bus mutex (lazily created; never fails on ESP32-S3). */
inline SemaphoreHandle_t mutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

/** Take the bus (blocks until free). */
inline void take() { xSemaphoreTake(mutex(), portMAX_DELAY); }

/** Non-blocking variant: true when the bus was taken, false if busy. */
inline bool tryTake() { return xSemaphoreTake(mutex(), 0) == pdTRUE; }

/** Give the bus back. */
inline void give() { xSemaphoreGive(mutex()); }

} // namespace spi_bus_guard

#endif // CATFISH_SPI_BUS_GUARD_H
