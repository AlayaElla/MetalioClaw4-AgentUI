#include "mipi_dsi_power_control.h"

#include <string.h>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "MipiDsiPower";

static esp_pm_lock_handle_t s_dsi_dpi_lock;
static bool s_initialized;
static bool s_idle;
static bool s_panel_present;
static bool s_lock_held;
static int s_applied_max_freq_mhz;
static int s_requested_max_freq_mhz = 360;
static bool s_standby_sleep_allowed;
static bool s_applied_light_sleep;
static portMUX_TYPE s_sleep_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static metalio_sleep_stats_t s_sleep_stats;

esp_err_t __real_esp_light_sleep_start(void);
esp_err_t IRAM_ATTR __wrap_esp_light_sleep_start(void)
{
    const int64_t start_us = esp_timer_get_time();
    const esp_err_t err = __real_esp_light_sleep_start();
    const uint64_t elapsed_us = esp_timer_get_time() - start_us;
    portENTER_CRITICAL(&s_sleep_stats_mux);
    if (err == ESP_OK) {
        ++s_sleep_stats.entries;
        // Successful sleep calls only; includes their entry/exit overhead.
        s_sleep_stats.slept_us += elapsed_us;
    } else {
        ++s_sleep_stats.rejected;
    }
    portEXIT_CRITICAL(&s_sleep_stats_mux);
    return err;
}

metalio_sleep_stats_t metalio_mipi_dsi_power_sleep_stats(void)
{
    portENTER_CRITICAL(&s_sleep_stats_mux);
    const metalio_sleep_stats_t stats = s_sleep_stats;
    portEXIT_CRITICAL(&s_sleep_stats_mux);
    return stats;
}

static esp_err_t apply_frequency_profile(int max_freq_mhz)
{
    const bool light_sleep = s_standby_sleep_allowed && !s_panel_present &&
                             max_freq_mhz == 40;
    if (s_initialized && s_applied_max_freq_mhz == max_freq_mhz &&
        s_applied_light_sleep == light_sleep) {
        return ESP_OK;
    }
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = max_freq_mhz,
        .min_freq_mhz = 40,
        // Tickless idle wakes at the next task deadline, including the
        // IO-expander side-key poll. Peripheral transactions retain PM locks.
        .light_sleep_enable = light_sleep,
    };
    const esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frequency profile %d MHz failed: %s", max_freq_mhz,
                 esp_err_to_name(err));
        return err;
    }
    s_applied_max_freq_mhz = max_freq_mhz;
    s_applied_light_sleep = light_sleep;
    ESP_LOGI(TAG, "frequency profile=%d MHz, current=%d MHz", max_freq_mhz,
             esp_clk_cpu_freq() / 1000000);
    return ESP_OK;
}

static int effective_max_freq_mhz(void)
{
    return s_panel_present && s_requested_max_freq_mhz < 360
               ? 360
               : s_requested_max_freq_mhz;
}

esp_err_t metalio_mipi_dsi_power_set_standby_sleep(bool enabled)
{
    if (!s_initialized || (enabled &&
        (s_panel_present || s_requested_max_freq_mhz != 40))) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool previous = s_standby_sleep_allowed;
    s_standby_sleep_allowed = enabled;
    const esp_err_t err = apply_frequency_profile(effective_max_freq_mhz());
    if (err != ESP_OK) s_standby_sleep_allowed = previous;
    return err;
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

// ESP-IDF 6.0.2 keeps a CPU_FREQ_MAX lock for the lifetime of a DPI panel.
// Capture its handle so the board can clear it after the public panel delete API
// releases it and replace it when the panel is recreated.
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
        s_panel_present = true;
        s_idle = false;
        s_lock_held = true;
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
    s_requested_max_freq_mhz = 360;
    const esp_err_t err = apply_frequency_profile(effective_max_freq_mhz());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DFS configuration failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "DFS ready: panel-present=%d applied-max=%d MHz current=%d MHz",
             s_panel_present, s_applied_max_freq_mhz,
             esp_clk_cpu_freq() / 1000000);
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
    s_requested_max_freq_mhz = max_freq_mhz;
    if (max_freq_mhz > 40) s_standby_sleep_allowed = false;
    const int applied = effective_max_freq_mhz();
    const esp_err_t err = apply_frequency_profile(applied);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "frequency request=%d applied-max=%d current=%d MHz panel-present=%d",
                 s_requested_max_freq_mhz, s_applied_max_freq_mhz,
                 esp_clk_cpu_freq() / 1000000, s_panel_present);
    }
    return err;
}

void metalio_mipi_dsi_power_panel_deleted(void)
{
    s_panel_present = false;
    s_idle = true;
    s_lock_held = false;
    s_dsi_dpi_lock = NULL;
    ESP_LOGI(TAG, "DPI panel deleted; driver bandwidth lock released");
}

esp_err_t metalio_mipi_dsi_power_set_idle(bool idle)
{
    if (!s_initialized) {
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

    if (!s_panel_present || s_dsi_dpi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = apply_frequency_profile(effective_max_freq_mhz());
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
    ESP_LOGI(TAG,
             "panel awake; request=%d applied-max=%d current=%d MHz",
             s_requested_max_freq_mhz, s_applied_max_freq_mhz,
             esp_clk_cpu_freq() / 1000000);
    return ESP_OK;
}
