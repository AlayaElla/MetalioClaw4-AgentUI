#include "battery_sampling_service.h"

#include <algorithm>
#include <limits>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr char kTag[] = "BatterySampler";
}

bool BatterySamplingService::Start(void* context, Sampler sampler,
                                   SamplePolicy sample_policy,
                                   uint32_t sample_period_ms,
                                   uint32_t max_age_ms) {
    if (sampler == nullptr || sample_period_ms == 0 || max_age_ms == 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (started_) return task_ != nullptr;
        context_ = context;
        sampler_ = sampler;
        sample_policy_ = sample_policy;
        sample_period_ms_ = sample_period_ms;
        max_age_ms_ = max_age_ms;
        paused_ = false;
        sample_requested_ = true;
        started_ = true;
    }

    TaskHandle_t task = nullptr;
    constexpr uint32_t kTaskStackBytes = 4096;
    BaseType_t task_created = xTaskCreateWithCaps(
        TaskEntry, "battery_sample", kTaskStackBytes, this, 3, &task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_created != pdPASS) {
        ESP_LOGW(kTag, "PSRAM task stack unavailable; trying internal RAM");
        task_created = xTaskCreateWithCaps(
            TaskEntry, "battery_sample", kTaskStackBytes, this, 3, &task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (task_created != pdPASS) {
        ESP_LOGE(kTag, "Failed to allocate battery sampling task stack");
        std::lock_guard<std::mutex> lock(state_mutex_);
        started_ = false;
        context_ = nullptr;
        sampler_ = nullptr;
        sample_policy_ = nullptr;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        task_ = task;
    }
    return true;
}

bool BatterySamplingService::GetSnapshot(BatterySnapshot& snapshot) const {
    return cache_.Read(esp_timer_get_time(), max_age_ms_, snapshot);
}

bool BatterySamplingService::PauseAndWait(uint32_t timeout_ms) {
    TaskHandle_t task = nullptr;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!started_) return true;
        paused_ = true;
        sample_requested_ = false;
        task = task_;
    }
    if (task != nullptr) xTaskNotifyGive(task);

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!sample_in_progress_) return true;
        }
        if (static_cast<TickType_t>(xTaskGetTickCount() - start) >= timeout) {
            return false;
        }
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(5)));
    }
}

void BatterySamplingService::ResumeAndSampleSoon() {
    TaskHandle_t task = nullptr;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!started_) return;
        paused_ = false;
        sample_requested_ = true;
        task = task_;
    }
    if (task != nullptr) xTaskNotifyGive(task);
}

void BatterySamplingService::RequestSampleSoon() {
    TaskHandle_t task = nullptr;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!started_ || paused_) return;
        sample_requested_ = true;
        task = task_;
    }
    if (task != nullptr) xTaskNotifyGive(task);
}

void BatterySamplingService::TaskEntry(void* context) {
    auto* service = static_cast<BatterySamplingService*>(context);
    if (service != nullptr) service->Run();
    vTaskDelete(nullptr);
}

void BatterySamplingService::Run() {
    int64_t next_sample_us = 0;
    while (true) {
        bool paused = false;
        bool requested = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            paused = paused_;
            requested = sample_requested_;
        }
        if (paused) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const bool allowed = sample_policy_ == nullptr ||
                             sample_policy_(context_);
        const int64_t now_us = esp_timer_get_time();
        if (!allowed) {
            next_sample_us = 0;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kInactiveCheckPeriodMs));
            continue;
        }

        if (requested || next_sample_us == 0 || now_us >= next_sample_us) {
            bool should_sample = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!paused_ && !sample_in_progress_) {
                    sample_requested_ = false;
                    sample_in_progress_ = true;
                    should_sample = true;
                }
            }
            if (!should_sample) continue;

            BatteryReading reading{};
            const bool sampled = sampler_(context_, reading);
            if (sampled && reading.voltage_valid) {
                cache_.Publish(reading, esp_timer_get_time());
            }
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                sample_in_progress_ = false;
            }
            next_sample_us = esp_timer_get_time() +
                             static_cast<int64_t>(sample_period_ms_) * 1000;
            continue;
        }

        const int64_t remaining_us = next_sample_us - now_us;
        const uint32_t remaining_ms = static_cast<uint32_t>(
            std::max<int64_t>(1, (remaining_us + 999) / 1000));
        const uint32_t wait_ms = std::min(remaining_ms, kInactiveCheckPeriodMs);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}
