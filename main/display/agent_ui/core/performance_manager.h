#pragma once

#include <cstdint>

class Board;

namespace agent_ui {

enum class PerformanceDemand : uint32_t {
    Ai = 1U << 0,
    Camera = 1U << 1,
    RealtimeAudio = 1U << 2,
    Media = 1U << 3,
    Transfer = 1U << 4,
    Custom0 = 1U << 16,
    Custom1 = 1U << 17,
};

enum class StandbyPerformancePhase : uint8_t {
    Awake,
    Dim,
    ScreenOff,
};

struct PerformancePolicy {
    int boost_mhz = 360;
    int idle_mhz = 180;
    int long_idle_mhz = 90;
    int screen_off_mhz = 40;
    uint8_t long_idle_brightness_cap_percent = 30;
    uint8_t cpu_load_guard_enter_percent = 80;
    uint8_t cpu_load_guard_exit_percent = 50;
    uint32_t boost_animation_period_ms = 33;
    uint32_t idle_animation_period_ms = 66;
    uint32_t long_idle_animation_period_ms = 125;
    uint32_t boost_hold_ms = 4000;
    uint32_t long_idle_after_ms = 60000;
};

// Central policy owner for CPU frequency decisions. Feature modules publish
// activity/demand only; the board layer applies the selected ceiling.
class PerformanceManager {
public:
    static PerformanceManager& Get();

    void Initialize(Board& board);
    void NotifyActivity();
    void SetDemand(PerformanceDemand demand, bool active);
    void SetStandbyPhase(StandbyPerformancePhase phase);
    bool SetPolicy(const PerformancePolicy& policy);
    void Tick();

    const PerformancePolicy& policy() const { return policy_; }
    int current_max_mhz() const { return current_max_mhz_; }
    uint8_t current_cpu_load_percent() const { return cpu_load_percent_; }
    uint32_t animation_frame_period_ms() const;

private:
    PerformanceManager() = default;
    static uint64_t NowMs();
    void UpdateCpuLoad();
    void Evaluate();
    void ApplyMaxFrequency(int max_freq_mhz, const char* reason);
    void ApplyLongIdleBrightness(bool long_idle);

    PerformancePolicy policy_;
    Board* board_ = nullptr;
    uint64_t last_activity_ms_ = 0;
    uint32_t demand_mask_ = 0;
    StandbyPerformancePhase standby_phase_ =
        StandbyPerformancePhase::Awake;
    int current_requested_mhz_ = -1;
    int current_max_mhz_ = -1;
    const char* current_reason_ = nullptr;
    uint64_t last_load_sample_us_ = 0;
    uint64_t last_idle_runtime_us_ = 0;
    uint8_t cpu_load_percent_ = 0;
    bool cpu_load_guard_active_ = false;
    bool long_idle_brightness_applied_ = false;
    bool initialized_ = false;
};

}  // namespace agent_ui
