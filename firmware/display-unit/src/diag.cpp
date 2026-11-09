/**
 * @file diag.cpp
 * @brief Boot-time heap telemetry (v1.11.4) — implementation.
 */
#include "diag.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

namespace diag {

void heapReport(const char *tag) {
  const uint32_t intFree =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t intMax =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t psFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const uint32_t psMax = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const uint32_t minEver = esp_get_minimum_free_heap_size();
  Serial.printf("[HEAP] %-16s internal free=%6u largest=%6u | psram free=%7u "
                "largest=%7u | min-ever=%u\n",
                tag, (unsigned)intFree, (unsigned)intMax, (unsigned)psFree,
                (unsigned)psMax, (unsigned)minEver);
}

} // namespace diag
