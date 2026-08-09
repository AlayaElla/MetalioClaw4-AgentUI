#include "performance_manager.h"

#include <algorithm>
#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "backlight.h"
#include "board.h"

namespace agent_ui {
namespace {
constexpr char kTag[] = "AgentPerformance";

uint32_t DemandBit(PerformanceDemand demand) {
    return static_cast<uint32_t>(demand);
}

constexpr uint32_t kBoostDemandMask =
    static_cast<uint32_t>(PerformanceDemand::Ai) |
    static_cast<uint32_t>(PerformanceDemand::Camera) |
    static_cast<uint32_t>(PerformanceDemand::Transfer) |
    static_cast<uint32_t>(PerformanceDemand::Custom0);
constexpr uint32_t kBalancedDemandMask =
    static_cast<uint32_t>(PerformanceDemand::RealtimeAudio) |
    static_cast<uint32_t>(PerformanceDemand::Media) |
    static_cast<uint32_t>(PerformanceDemand::Custom1);
}  // namespace

PerformanceManager& PerformanceManager::Get() {
    static PerformanceManager instance;
    return instance;
}

uint64_t PerformanceManager::NowMs() {
    return static_cast<uint64_t>(esp_timer_get_time()) / 1000U;
}

void PerformanceManager::Initialize(Board& board) {
    if (initialized_) return;
    board_ = &board;
    initialized_ = true;
    last_activity_ms_ = NowMs();
    last_load_sample_us_ = static_cast<uint64_t>(esp_timer_get_time());
    last_idle_runtime_us_ =
        static_cast<uint64_t>(ulTaskGetIdleRunTimeCounter());
    Evaluate();
}

void PerformanceManager::NotifyActivity() {
    last_activity_ms_ = NowMs();
    if (!initialized_) return;
    Evaluate();
}

void PerformanceManager::SetDemand(PerformanceDemand demand, bool active) {
    const uint32_t bit = DemandBit(demand);
    const uint32_t next_mask =
        active ? (demand_mask_ | bit) : (demand_mask_ & ~bit);
    if (next_mask == demand_mask_) return;
    demand_mask_ = next_mask;
    // Hold boost briefly after a workload starts or finishes to avoid rapid
    // oscillation around state transitions.
    last_activity_ms_ = NowMs();
    if (!initialized_) return;
    Evaluate();
}

void PerformanceManager::SetStandbyPhase(StandbyPerformancePhase phase) {
    if (standby_phase_ == phase) return;
    standby_phase_ = phase;
    if (phase == StandbyPerformancePhase::Awake) {
        last_activity_ms_ = NowMs();
    }
    if (!initialized_) return;
    Evaluate();
}

bool PerformanceManager::SetPolicy(const PerformancePolicy& policy) {
    if (policy.screen_off_mhz <= 0 ||
        policy.screen_off_mhz > policy.long_idle_mhz ||
        policy.long_idle_mhz > policy.idle_mhz ||
        policy.idle_mhz > policy.boost_mhz ||
        policy.long_idle_brightness_cap_percent == 0 ||
        policy.long_idle_brightness_cap_percent > 100 ||
        policy.cpu_load_guard_exit_percent >=
            policy.cpu_load_guard_enter_percent ||
        policy.cpu_load_guard_enter_percent > 100 ||
        policy.boost_animation_period_ms == 0 ||
        policy.idle_animation_period_ms <
            policy.boost_animation_period_ms ||
        policy.long_idle_animation_period_ms <
            policy.idle_animation_period_ms ||
        policy.boost_hold_ms > policy.long_idle_after_ms) {
        ESP_LOGE(kTag, "Rejected invalid performance policy");
        return false;
    }
    policy_ = policy;
    last_activity_ms_ = NowMs();
    if (!initialized_) return true;
    Evaluate();
    return true;
}

void PerformanceManager::Tick() {
    if (!initialized_) return;
    UpdateCpuLoad();
    Evaluate();
}

uint32_t PerformanceManager::animation_frame_period_ms() const {
    // Animation pacing follows the logical policy tier even when the board
    // must clamp the actual CPU ceiling to keep an active display stable.
    if (current_requested_mhz_ < 0 ||
        current_requested_mhz_ >= policy_.boost_mhz) {
        return policy_.boost_animation_period_ms;
    }
    if (current_requested_mhz_ >= policy_.idle_mhz) {
        return policy_.idle_animation_period_ms;
    }
    return policy_.long_idle_animation_period_ms;
}

void PerformanceManager::UpdateCpuLoad() {
    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t idle_runtime_us =
        static_cast<uint64_t>(ulTaskGetIdleRunTimeCounter());
    if (last_load_sample_us_ == 0 || now_us <= last_load_sample_us_ ||
        idle_runtime_us < last_idle_runtime_us_) {
        last_load_sample_us_ = now_us;
        last_idle_runtime_us_ = idle_runtime_us;
        return;
    }

    const uint64_t elapsed_us = now_us - last_load_sample_us_;
    const uint64_t idle_us = idle_runtime_us - last_idle_runtime_us_;
    const uint64_t available_us = elapsed_us * configNUMBER_OF_CORES;
    const uint64_t idle_percent = available_us == 0
                                      ? 0
                                      : std::min<uint64_t>(
                                            100, idle_us * 100 / available_us);
    cpu_load_percent_ = static_cast<uint8_t>(100 - idle_percent);
    last_load_sample_us_ = now_us;
    last_idle_runtime_us_ = idle_runtime_us;

    const bool previous = cpu_load_guard_active_;
    if (cpu_load_percent_ >= policy_.cpu_load_guard_enter_percent) {
        cpu_load_guard_active_ = true;
    } else if (cpu_load_percent_ <= policy_.cpu_load_guard_exit_percent) {
        cpu_load_guard_active_ = false;
    }
    if (previous != cpu_load_guard_active_) {
        ESP_LOGI(kTag, "CPU load guard=%d load=%u%%",
                 cpu_load_guard_active_,
                 static_cast<unsigned>(cpu_load_percent_));
    }
}

void PerformanceManager::Evaluate() {
    int target_mhz = policy_.boost_mhz;
    const char* reason = "startup";
    if (standby_phase_ == StandbyPerformancePhase::ScreenOff) {
        target_mhz = policy_.screen_off_mhz;
        reason = "screen_off";
    } else if (standby_phase_ == StandbyPerformancePhase::Dim) {
        target_mhz = policy_.idle_mhz;
        reason = "standby_dim";
    } else {
        const uint64_t idle_ms = NowMs() - last_activity_ms_;
        if ((demand_mask_ & kBoostDemandMask) != 0) {
            target_mhz = policy_.boost_mhz;
            reason = "foreground_workload";
        } else if (idle_ms < policy_.boost_hold_ms) {
            target_mhz = policy_.boost_mhz;
            reason = "recent_activity";
        } else if ((demand_mask_ & kBalancedDemandMask) != 0) {
            target_mhz = policy_.idle_mhz;
            reason = "realtime_balanced";
        } else if (idle_ms < policy_.long_idle_after_ms) {
            target_mhz = policy_.idle_mhz;
            reason = "visible_idle";
        } else if (cpu_load_guard_active_) {
            target_mhz = policy_.idle_mhz;
            reason = "cpu_load_guard";
        } else {
            target_mhz = policy_.long_idle_mhz;
            reason = "long_idle";
        }
    }
    ApplyMaxFrequency(target_mhz, reason);
    ApplyLongIdleBrightness(
        standby_phase_ == StandbyPerformancePhase::Awake &&
        target_mhz == policy_.long_idle_mhz);
}

void PerformanceManager::ApplyMaxFrequency(int requested_mhz,
                                           const char* reason) {
    if (board_ == nullptr) return;

    const int display_floor_mhz =
        standby_phase_ == StandbyPerformancePhase::ScreenOff
            ? board_->GetScreenOffMinMhz()
            : board_->GetActiveDisplayMinMhz();
    const int applied_mhz = std::max(requested_mhz, display_floor_mhz);
    const bool request_changed = current_requested_mhz_ != requested_mhz;
    const bool frequency_changed = current_max_mhz_ != applied_mhz;
    const bool reason_changed = current_reason_ == nullptr ||
                                std::strcmp(current_reason_, reason) != 0;
    if (!request_changed && !frequency_changed && !reason_changed) return;
    if (frequency_changed) {
        board_->SetPerformanceMaxMhz(applied_mhz);
        current_max_mhz_ = applied_mhz;
    }
    current_requested_mhz_ = requested_mhz;
    current_reason_ = reason;
    const uint64_t idle_ms = NowMs() - last_activity_ms_;
    ESP_LOGI(kTag,
             "profile=%s requested=%dMHz applied=%dMHz display_floor=%dMHz "
             "idle=%llums load=%u%% demands=0x%08lx phase=%u animation=%ums",
             reason, requested_mhz, applied_mhz, display_floor_mhz,
             static_cast<unsigned long long>(idle_ms),
             static_cast<unsigned>(cpu_load_percent_),
             static_cast<unsigned long>(demand_mask_),
             static_cast<unsigned>(standby_phase_),
             static_cast<unsigned>(animation_frame_period_ms()));
}

void PerformanceManager::ApplyLongIdleBrightness(bool long_idle) {
    if (board_ == nullptr) return;
    Backlight* backlight = board_->GetBacklight();
    if (backlight == nullptr) return;

    if (long_idle) {
        if (long_idle_brightness_applied_) return;
        const uint8_t current = backlight->brightness();
        const uint8_t reduced = current > kBacklightMinPercent
                                    ? std::max<uint8_t>(
                                          kBacklightMinPercent, current / 2)
                                    : current;
        const uint8_t target = std::min(
            reduced, policy_.long_idle_brightness_cap_percent);
        if (target < current) {
            backlight->SetBrightness(target, false);
            ESP_LOGI(kTag, "Long-idle brightness=%u%%",
                     static_cast<unsigned>(target));
        }
        long_idle_brightness_applied_ = true;
        return;
    }

    if (long_idle_brightness_applied_ &&
        standby_phase_ == StandbyPerformancePhase::Awake) {
        long_idle_brightness_applied_ = false;
        backlight->RestoreBrightness();
        ESP_LOGI(kTag, "Restored configured brightness");
    }
}

}  // namespace agent_ui
