#include "ui_dispatcher.h"

#include "lvgl.h"

#include <atomic>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <memory>
#include <new>
#include <utility>

namespace {
constexpr const char* kTag = "UiDispatcher";
constexpr UBaseType_t kQueueLength = 32;
constexpr uint32_t kDrainPeriodMs = 10;
constexpr int kMaxTasksPerTick = 8;

using Task = std::function<void()>;

std::atomic<QueueHandle_t> s_queue{nullptr};
lv_timer_t* s_timer = nullptr;

void DrainQueue(lv_timer_t* /*timer*/) {
    QueueHandle_t queue = s_queue.load();
    if (!queue) return;

    for (int i = 0; i < kMaxTasksPerTick; ++i) {
        Task* raw_task = nullptr;
        if (xQueueReceive(queue, &raw_task, 0) != pdTRUE) break;

        std::unique_ptr<Task> task(raw_task);
        if (*task) (*task)();
    }
}
}  // namespace

bool UiDispatcher::Init() {
    if (!s_queue.load()) {
        QueueHandle_t queue = xQueueCreate(kQueueLength, sizeof(Task*));
        if (!queue) {
            ESP_LOGE(kTag, "Failed to create UI task queue");
            return false;
        }
        s_queue.store(queue);
    }

    if (!s_timer) {
        s_timer = lv_timer_create(DrainQueue, kDrainPeriodMs, nullptr);
        if (!s_timer) {
            ESP_LOGE(kTag, "Failed to create UI drain timer");
            return false;
        }
    }
    return true;
}

bool UiDispatcher::Post(std::function<void()> callback) {
    QueueHandle_t queue = s_queue.load();
    if (!queue || !callback) return false;

    auto* task = new (std::nothrow) Task(std::move(callback));
    if (!task) return false;

    if (xQueueSend(queue, &task, 0) != pdTRUE) {
        delete task;
        ESP_LOGW(kTag, "UI task queue full; dropping update");
        return false;
    }
    return true;
}
