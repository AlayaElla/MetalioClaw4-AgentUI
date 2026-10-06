#pragma once

#include <cstdint>

namespace agent_ui::power_diagnostics {

inline constexpr int kTargetStandbyHours = 24;
inline constexpr int kBatteryCapacityMah = 1500;
inline constexpr int kUsableCapacityMah = 1200;

// Deadlines use milliseconds elapsed since the screen became black. An early
// RTOS wake waits only for the remaining time, never skips an entire sample.
class SamplingWindow {
public:
    explicit SamplingWindow(uint32_t duration_ms) : duration_ms_(duration_ms) {}
    bool Due(uint32_t elapsed_ms) const {
        return elapsed_ms < duration_ms_ && elapsed_ms >= next_sample_ms_;
    }
    void SampleAttempted(uint32_t elapsed_ms) {
        // If execution was delayed, take one fresh reading rather than a
        // burst of repeated readings to fill elapsed slots.
        next_sample_ms_ = (elapsed_ms / 1000U + 1U) * 1000U;
    }
    uint32_t WaitMs(uint32_t elapsed_ms) const {
        const uint32_t deadline = next_sample_ms_ < duration_ms_
                                      ? next_sample_ms_ : duration_ms_;
        return elapsed_ms < deadline ? deadline - elapsed_ms : 0;
    }
private:
    uint32_t duration_ms_;
    uint32_t next_sample_ms_ = 2000;
};

enum class TestStage : uint8_t {
    Idle,
    WaitingForScreenOff,
    Settling,
    Measuring,
    WaitingForWake,
    Complete,
    Cancelled,
};

struct TestResult {
    uint32_t black_screen_mhz = 0;
    uint32_t elapsed_ms = 0;
    bool woke_to_lock_screen = false;
};

// Only valid samples belong in the average. A charger observed even once
// means this window cannot be interpreted as battery-only standby draw.
enum class PowerBudget {
    Unavailable,
    NeedsOptimization,
    NominalOnly,
    WithReserve,
};

class PowerSamples {
public:
    void Add(int current_ma, bool external_power) {
        sum_ma_ += current_ma;
        ++count_;
        saw_external_power_ = saw_external_power_ || external_power;
        saw_charging_ = saw_charging_ || current_ma > 0;
    }
    uint32_t count() const { return count_; }
    int average_ma() const { return count_ ? static_cast<int>(sum_ma_ / count_) : 0; }
    bool battery_only() const { return count_ != 0 && !saw_external_power_; }
    PowerBudget budget() const {
        // Compare the unrounded mean, so 50.5 mA cannot pass as 50 mA.
        // Positive or zero net current cannot establish a discharge budget.
        if (!battery_only() || saw_charging_ || sum_ma_ >= 0) return PowerBudget::Unavailable;
        const int64_t current_sum = -sum_ma_;
        if (current_sum * kTargetStandbyHours <= int64_t{kUsableCapacityMah} * count_)
            return PowerBudget::WithReserve;
        if (current_sum * kTargetStandbyHours <= int64_t{kBatteryCapacityMah} * count_)
            return PowerBudget::NominalOnly;
        return PowerBudget::NeedsOptimization;
    }
private:
    int64_t sum_ma_ = 0;
    uint32_t count_ = 0;
    bool saw_external_power_ = false;
    bool saw_charging_ = false;
};

// Small time/state model kept independent from LVGL and the firmware timers so
// cancellation and measurement ordering can be exercised on the host.
class Controller {
public:
    void Start(uint64_t started_ms) {
        result_ = {};
        started_ms_ = started_ms;
        stage_ = TestStage::WaitingForScreenOff;
    }

    bool ScreenTurnedOff() {
        if (stage_ != TestStage::WaitingForScreenOff) return false;
        stage_ = TestStage::Settling;
        return true;
    }

    bool BeginMeasurement() {
        if (stage_ != TestStage::Settling) return false;
        stage_ = TestStage::Measuring;
        return true;
    }

    bool RecordBlackScreenFrequency(uint32_t mhz) {
        if (stage_ != TestStage::Measuring || mhz == 0) return false;
        result_.black_screen_mhz = mhz;
        stage_ = TestStage::WaitingForWake;
        return true;
    }

    bool Woke(uint64_t now_ms, bool lock_screen_active) {
        if (stage_ != TestStage::WaitingForWake) return false;
        result_.elapsed_ms = now_ms >= started_ms_
                                 ? static_cast<uint32_t>(now_ms - started_ms_)
                                 : 0;
        result_.woke_to_lock_screen = lock_screen_active;
        stage_ = TestStage::Complete;
        return true;
    }

    void Cancel() {
        if (stage_ == TestStage::Complete || stage_ == TestStage::Idle) return;
        stage_ = TestStage::Cancelled;
    }

    TestStage stage() const { return stage_; }
    const TestResult& result() const { return result_; }

private:
    TestStage stage_ = TestStage::Idle;
    uint64_t started_ms_ = 0;
    TestResult result_{};
};

}  // namespace agent_ui::power_diagnostics
