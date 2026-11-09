/**
 * @file diag.h
 * @brief Boot-time heap telemetry (v1.11.4).
 *
 * One-line heap snapshots at boot milestones so internal-RAM starvation
 * (the esp_wifi_init ESP_ERR_NO_MEM class of failure) is visible in the
 * serial log on real hardware and in the QEMU smoke run alike.
 */
#pragma once

namespace diag {

/** Print free/largest internal + PSRAM and min-ever free heap as one line. */
void heapReport(const char *tag);

} // namespace diag
