/**
 * @file catfish_lv_mem.h
 * @brief PSRAM-backed allocator for the LVGL widget pool (v1.11.4).
 *
 * WHY: lv_conf.h previously used the built-in 160 KB LV_MEM_SIZE pool, which
 * is a static array in internal DRAM. The ESP32-S3 has only ~338 KB usable
 * internal RAM; static (239 KB with the pool) + task stacks (~58 KB) left
 * well under 10 KB free by the time the WiFi driver initialized, so
 * esp_wifi_init() failed with ESP_ERR_NO_MEM and could not even create its
 * task/event stacks — the WiFi setup portal (CatfishFeeder-Setup) never
 * appeared. The widget pool does not need internal RAM: widgets live there
 * only as metadata, pixels already render through the PSRAM direct-mode
 * buffers, so the pool now lives in PSRAM (8 MB on the N16R8 board).
 *
 * The heap_caps_malloc(MALLOC_CAP_SPIRAM) call falls back to internal RAM
 * when PSRAM is unavailable — that keeps the headless QEMU build (no PSRAM
 * emulation) and any PSRAM-less variant functional.
 */
#pragma once

#include <esp_heap_caps.h>
#include <stddef.h>

/* lv_mem.c is C: bare `inline` in C99 emits no external definition, which
   breaks the link. `static inline` gives each C TU its own copy; C++ keeps
   the standard external-linkage inline. */
#ifdef __cplusplus
#define CATFISH_LV_MEM_INLINE inline
#else
#define CATFISH_LV_MEM_INLINE static inline
#endif

CATFISH_LV_MEM_INLINE void *catfish_lv_mem_alloc(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
  return p;
}

CATFISH_LV_MEM_INLINE void *catfish_lv_mem_realloc(void *ptr, size_t size) {
  void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p && size) p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
  return p;
}

CATFISH_LV_MEM_INLINE void catfish_lv_mem_free(void *ptr) {
  heap_caps_free(ptr);
}
