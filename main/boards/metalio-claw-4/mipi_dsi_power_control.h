#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configure dynamic frequency scaling after the MIPI-DPI panel has been
// created. Policy selection lives in PerformanceManager; this module only
// applies the requested CPU ceiling and controls the DSI stream lock.
esp_err_t metalio_mipi_dsi_power_init(void);

// Accept one of the board-supported ceilings: 40, 90, 180, or 360 MHz. All
// requests are clamped to 360 MHz because panel sleep does not stop the
// fixed-rate RGB888 DPI stream and hardware testing underruns at 180 MHz.
esp_err_t metalio_mipi_dsi_power_set_frequency(int max_freq_mhz);

// Track panel sleep without releasing the display driver's CPU bandwidth lock.
// The RGB888 framebuffer stream remains continuous and phase-aligned.
esp_err_t metalio_mipi_dsi_power_set_idle(bool idle);

#ifdef __cplusplus
}
#endif
