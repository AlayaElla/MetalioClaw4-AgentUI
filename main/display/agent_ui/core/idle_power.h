#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "lvgl.h"
#include "micro_display_policy.h"

class Board;

namespace agent_ui {

class IdlePower {
public:
    struct HomeCallbacks {
        std::function<void()> notify_user_activity;
        std::function<void()> sleep_expression;
        std::function<bool()> is_mounted;
        std::function<void(bool, int, bool)> update_battery;
    };

    static constexpr int kDefaultStandbyMinutes = 5;
    static constexpr std::array<int, 5> kStandbyMinuteOptions = {
        1, 5, 10, 15, 30,
    };

    static IdlePower& Get();

    void Initialize(Board& board);
    void SetHomeCallbacks(HomeCallbacks callbacks);
    void NotifyActivity();
    void SetMicroDisplay(const MicroDisplayConfig& config);
    void RestoreExpressionSleep();
    void SetStandbyActive(bool active);
    int standby_minutes() const;
    void SetStandbyMinutes(int minutes);

    static int NormalizeStandbyMinutes(int minutes);

private:
    IdlePower() = default;
    static void TimerCallback(lv_timer_t* timer);
    void Tick();
    void UpdateMicroBacklight();
    MicroDisplayPolicy micro_display_;
    HomeCallbacks home_callbacks_;
    int micro_brightness_ = -1;

    lv_timer_t* timer_ = nullptr;
    uint32_t last_activity_tick_ = 0;
    uint32_t last_battery_refresh_tick_ = 0;
    int standby_minutes_ = -1;
    bool standby_active_ = false;
    bool expression_sleep_triggered_ = false;
};

}  // namespace agent_ui
