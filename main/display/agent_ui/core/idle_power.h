#pragma once

#include <array>
#include <cstdint>

#include "lvgl.h"

class Board;

namespace agent_ui {

class IdlePower {
public:
    static constexpr int kDefaultStandbyMinutes = 5;
    static constexpr std::array<int, 5> kStandbyMinuteOptions = {
        1, 5, 10, 15, 30,
    };

    static IdlePower& Get();

    void Initialize(Board& board);
    void NotifyActivity();
    void RestoreExpressionSleep();
    void SetStandbyActive(bool active);
    int standby_minutes() const;
    void SetStandbyMinutes(int minutes);

    static int NormalizeStandbyMinutes(int minutes);

private:
    IdlePower() = default;
    static void TimerCallback(lv_timer_t* timer);
    void Tick();

    lv_timer_t* timer_ = nullptr;
    uint32_t last_activity_tick_ = 0;
    uint32_t last_battery_refresh_tick_ = 0;
    int standby_minutes_ = -1;
    bool standby_active_ = false;
    bool expression_sleep_triggered_ = false;
};

}  // namespace agent_ui
