#include "mipi_dsi_power_control.h"

#include <string.h>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"

static const char *TAG = "MipiDsiPower";

static esp_pm_lock_handle_t s_dsi_dpi_lock;
static bool s_initialized;
static bool s_idle;
static bool s_lock_held = true;
static int s_applied_max_freq_mhz;

static esp_err_t apply_frequency_profile(int max_freq_mhz)
{
    if (s_initialized && s_applied_max_freq_mhz == max_freq_mhz) {
        return ESP_OK;
    }
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = max_freq_mhz,
        .min_freq_mhz = 40,
        // PWR_KEY is currently delivered by an IO-expander task, not a
        // configured hardware light-sleep wake source.
        .light_sleep_enable = false,
    };
    const esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frequency profile %d MHz failed: %s", max_freq_mhz,
                 esp_err_to_name(err));
        return err;
    }
    s_applied_max_freq_mhz = max_freq_mhz;
    ESP_LOGI(TAG, "frequency profile=%d MHz, current=%d MHz", max_freq_mhz,
             esp_clk_cpu_freq() / 1000000);
    return ESP_OK;
}

static esp_err_t acquire_dsi_lock(void)
{
    if (s_lock_held) {
        return ESP_OK;
    }
    const esp_err_t err = esp_pm_lock_acquire(s_dsi_dpi_lock);
    if (err == ESP_OK) {
        s_lock_held = true;
    }
    return err;
}

// ESP-IDF 6.0.2 keeps a CPU_FREQ_MAX lock for the entire lifetime of a DPI
// panel. Capture only that driver's lock at link time so the board can verify
// that the continuous RGB888 stream retains its required bandwidth.
esp_err_t __real_esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg,
                                    const char *name,
                                    esp_pm_lock_handle_t *out_handle);

esp_err_t __wrap_esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg,
                                    const char *name,
                                    esp_pm_lock_handle_t *out_handle)
{
    const esp_err_t err =
        __real_esp_pm_lock_create(lock_type, arg, name, out_handle);
    if (err == ESP_OK && name != NULL && out_handle != NULL &&
        strcmp(name, "dsi_dpi") == 0) {
        s_dsi_dpi_lock = *out_handle;
    }
    return err;
}

esp_err_t metalio_mipi_dsi_power_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (s_dsi_dpi_lock == NULL) {
        ESP_LOGE(TAG, "DSI DPI power-management lock was not captured");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = apply_frequency_profile(360);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DFS configuration failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    s_lock_held = true;
    ESP_LOGI(TAG,
             "DFS ready: display-on=360 MHz, panel-sleep link continuous");
    return ESP_OK;
}

esp_err_t metalio_mipi_dsi_power_set_frequency(int max_freq_mhz)
{
    if (max_freq_mhz != 40 && max_freq_mhz != 90 &&
        max_freq_mhz != 180 && max_freq_mhz != 360) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Panel sleep only blanks the glass; the RGB888 DMA/DSI stream continues
    // reading PSRAM at the original pixel rate. It underruns at 180 MHz too,
    // so every requested tier must remain clamped to 360 MHz until the video
    // producer can be stopped safely by a public driver API.
    (void)max_freq_mhz;
    return apply_frequency_profile(360);
}

esp_err_t metalio_mipi_dsi_power_set_idle(bool idle)
{
    if (!s_initialized || s_dsi_dpi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_idle == idle) {
        return ESP_OK;
    }

    if (idle) {
        s_idle = true;
        ESP_LOGI(TAG,
                 "panel asleep; DSI link continuous, bandwidth lock retained, current=%d MHz",
                 esp_clk_cpu_freq() / 1000000);
        return ESP_OK;
    }

    // Keep this defensive in case a future driver path releases the captured
    // lock. The DSI bridge never stops, preserving RGB888 byte/line phase.
    esp_err_t err = apply_frequency_profile(360);
    if (err != ESP_OK) {
        return err;
    }
    err = acquire_dsi_lock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DSI CPU lock acquire failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    s_idle = false;
    ESP_LOGI(TAG, "panel wake bandwidth restored; DSI link remained continuous, current=%d MHz",
             esp_clk_cpu_freq() / 1000000);
    return ESP_OK;
}
