#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configure dynamic frequency scaling after the MIPI-DPI panel has been
// created. Policy selection lives in PerformanceManager; this module only
// applies the requested CPU ceiling and controls the DSI stream lock.
esp_err_t metalio_mipi_dsi_power_init(void);

// Accept one of the board-supported ceilings: 40, 90, 180, or 360 MHz. While
// the production DPI panel exists, its effective ceiling remains at 360 MHz.
esp_err_t metalio_mipi_dsi_power_set_frequency(int max_freq_mhz);

// Call only after esp_lcd_panel_del() succeeds. The IDF driver releases its
// captured DPI lock as part of deletion; this clears the now-stale handle.
void metalio_mipi_dsi_power_panel_deleted(void);

// Track panel sleep commands while its DPI producer remains active.
// These power-control calls are serialized by the display/performance paths;
// panel lifecycle callbacks run under the display adapter's lock.
esp_err_t metalio_mipi_dsi_power_set_idle(bool idle);

// Enable only after display, audio and radios have stopped. Any subsequent
// request above 40 MHz automatically disarms sleep before display recovery.
esp_err_t metalio_mipi_dsi_power_set_standby_sleep(bool enabled);
typedef struct {
    uint64_t slept_us;
    uint32_t entries;
    uint32_t rejected;
} metalio_sleep_stats_t;
metalio_sleep_stats_t metalio_mipi_dsi_power_sleep_stats(void);

#ifdef __cplusplus
}
#endif
