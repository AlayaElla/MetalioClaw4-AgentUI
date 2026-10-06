#pragma once

#include <cstdint>
#include "battery_snapshot_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Device-owned battery sampling task. The hardware callback is invoked only
// by this task and should return false when the voltage read failed. The
// optional policy callback lets a board stop polling while its display or
// peripherals are asleep without adding screen-off I2C traffic.
class BatterySamplingService {
public:
    using Sampler = bool (*)(void* context, BatteryReading& reading);
    using SamplePolicy = bool (*)(void* context);

    static constexpr uint32_t kDefaultSamplePeriodMs = 1000;
    static constexpr uint32_t kInactiveCheckPeriodMs = 10000;
    static constexpr uint32_t kDefaultMaxAgeMs = 10000;
    static constexpr uint32_t kPauseTimeoutMs = 300;

    BatterySamplingService() = default;
    BatterySamplingService(const BatterySamplingService&) = delete;
    BatterySamplingService& operator=(const BatterySamplingService&) = delete;

    bool Start(void* context, Sampler sampler, SamplePolicy sample_policy = nullptr,
               uint32_t sample_period_ms = kDefaultSamplePeriodMs,
               uint32_t max_age_ms = kDefaultMaxAgeMs);
    bool GetSnapshot(BatterySnapshot& snapshot) const;
    // PauseAndWait is for the board's background power worker. It never needs
    // the LVGL lock and fails closed if an in-flight bus read exceeds timeout.
    bool PauseAndWait(uint32_t timeout_ms = kPauseTimeoutMs);
    void ResumeAndSampleSoon();
    void RequestSampleSoon();

private:
    static void TaskEntry(void* context);
    void Run();

    void* context_ = nullptr;
    Sampler sampler_ = nullptr;
    SamplePolicy sample_policy_ = nullptr;
    uint32_t sample_period_ms_ = kDefaultSamplePeriodMs;
    uint32_t max_age_ms_ = kDefaultMaxAgeMs;
    TaskHandle_t task_ = nullptr;
    mutable std::mutex state_mutex_;
    bool paused_ = false;
    bool sample_requested_ = true;
    bool sample_in_progress_ = false;
    bool started_ = false;
    BatterySnapshotCache cache_;
};
