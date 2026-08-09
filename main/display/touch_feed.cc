#include "touch_feed.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

constexpr const char* kTag = "TouchFeed";
constexpr uint32_t kErrorBackoffMinMs = 100;
constexpr uint32_t kErrorBackoffMaxMs = 1000;

esp_lcd_touch_handle_t s_handle = nullptr;
SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_task = nullptr;
volatile bool s_run = false;
volatile bool s_paused = false;
uint32_t s_period_ms = 20;
void (*s_activity_callback)() = nullptr;

struct TouchSnapshot {
    bool pressed = false;
    int16_t x = 0;
    int16_t y = 0;
};

TouchSnapshot s_snap;

void ClearSnapshot() {
    if (s_mutex != nullptr &&
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_snap = TouchSnapshot{};
        xSemaphoreGive(s_mutex);
    }
}

#if TOUCH_FEED_DEBUG
bool s_log_was_pressed = false;
int s_log_last_x = -1;
int s_log_last_y = -1;

void LogSnapshotIfChanged(const TouchSnapshot& next) {
    if (!next.pressed) {
        if (s_log_was_pressed) {
            ESP_LOGW(kTag, "chip: released");
            s_log_was_pressed = false;
            s_log_last_x = -1;
            s_log_last_y = -1;
        }
        return;
    }

    const int dx = (s_log_last_x >= 0) ? (next.x - s_log_last_x) : 0;
    const int dy = (s_log_last_y >= 0) ? (next.y - s_log_last_y) : 0;
    const bool moved = !s_log_was_pressed || dx != 0 || dy != 0;
    if (moved) {
        ESP_LOGW(kTag, "chip: p0=(%d,%d) d=(%+d,%+d)%s", next.x, next.y, dx,
                 dy, s_log_was_pressed ? "" : " [down]");
    }

    s_log_was_pressed = true;
    s_log_last_x = next.x;
    s_log_last_y = next.y;
}
#endif

esp_err_t UpdateSnapshotFromChip() {
    if (s_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_lcd_touch_read_data(s_handle);
    if (err != ESP_OK) {
        return err;
    }

    TouchSnapshot next{};
    esp_lcd_touch_point_data_t points[1] = {};
    uint8_t cnt = 0;
    err = esp_lcd_touch_get_data(s_handle, points, &cnt, 1);
    if (err != ESP_OK) {
        return err;
    }

    if (cnt > 0) {
        next.pressed = true;
        next.x = static_cast<int16_t>(points[0].x);
        next.y = static_cast<int16_t>(points[0].y);
    } else {
        next.pressed = false;
    }

    if (s_mutex != nullptr &&
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_snap = next;
        xSemaphoreGive(s_mutex);
    }

#if TOUCH_FEED_DEBUG
    LogSnapshotIfChanged(next);
#endif
    return ESP_OK;
}

void ReaderTask(void* /*arg*/) {
#if TOUCH_FEED_DEBUG
    ESP_LOGI(kTag, "reader started, period=%u ms", s_period_ms);
#endif

    uint32_t consecutive_errors = 0;
    while (s_run) {
        if (s_paused) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const esp_err_t err = UpdateSnapshotFromChip();
        uint32_t delay_ms = s_period_ms;
        if (err == ESP_OK) {
            consecutive_errors = 0;
        } else {
            if (consecutive_errors++ == 0) {
                ClearSnapshot();
            }
            const uint32_t shift =
                consecutive_errors > 4 ? 4 : consecutive_errors - 1;
            delay_ms = kErrorBackoffMinMs << shift;
            if (delay_ms > kErrorBackoffMaxMs) {
                delay_ms = kErrorBackoffMaxMs;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    s_task = nullptr;
    vTaskDelete(nullptr);
}

void IndevReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    if (data == nullptr) {
        return;
    }

    TouchSnapshot snap{};
    if (s_mutex != nullptr &&
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snap = s_snap;
        xSemaphoreGive(s_mutex);
    }

    data->point.x = snap.x;
    data->point.y = snap.y;
    data->state =
        snap.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (snap.pressed && s_activity_callback != nullptr) {
        s_activity_callback();
    }
}

}  // namespace

void touch_feed_init(esp_lcd_touch_handle_t handle, uint32_t period_ms) {
    touch_feed_stop();

    s_handle = handle;
    s_paused = false;
    s_period_ms = (period_ms == 0) ? 20 : period_ms;

    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_mutex == nullptr) {
        ESP_LOGE(kTag, "mutex create failed");
        return;
    }

    {
        TouchSnapshot cleared;
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_snap = cleared;
            xSemaphoreGive(s_mutex);
        }
    }
#if TOUCH_FEED_DEBUG
    s_log_was_pressed = false;
    s_log_last_x = -1;
    s_log_last_y = -1;
#endif

    s_run = true;
    if (xTaskCreate(ReaderTask, "touch_feed", 4096, nullptr, 5, &s_task) !=
        pdPASS) {
        s_run = false;
        s_task = nullptr;
        ESP_LOGE(kTag, "xTaskCreate failed");
        return;
    }
}

void touch_feed_attach_indev(lv_indev_t* indev) {
    if (indev == nullptr) {
        ESP_LOGW(kTag, "attach_indev: null indev");
        return;
    }
    lv_indev_set_read_cb(indev, IndevReadCb);
}

void touch_feed_set_activity_callback(void (*callback)()) {
    s_activity_callback = callback;
}

void touch_feed_set_period(uint32_t period_ms) {
    s_period_ms = period_ms == 0 ? 20 : period_ms;
    ESP_LOGI(kTag, "reader period updated to %u ms", s_period_ms);
}

void touch_feed_pause() {
    if (s_task == nullptr || s_paused) {
        ClearSnapshot();
        return;
    }
    s_paused = true;
    ClearSnapshot();
    ESP_LOGI(kTag, "reader paused");
}

void touch_feed_resume() {
    if (!s_paused) return;
    s_paused = false;
    if (s_task != nullptr) xTaskNotifyGive(s_task);
    ESP_LOGI(kTag, "reader resumed");
}

void touch_feed_stop() {
    if (s_task != nullptr) {
        s_run = false;
        s_paused = false;
        xTaskNotifyGive(s_task);
        for (int i = 0; i < 50 && s_task != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_task != nullptr) {
            vTaskDelete(s_task);
            s_task = nullptr;
        }
    }
    s_run = false;
    s_paused = false;
    s_handle = nullptr;
}
