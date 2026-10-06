#pragma once

#include <cstdint>

#include "lvgl.h"

namespace agent_ui {

class StandbyView {
public:
    // Tests can enter black standby in this call, without the lock-screen timer.
    static void Show(bool screen_off_immediately = false);
    static void HandlePowerKey();
    // Completes only the wake side of a transition already prepared by a
    // non-LVGL task. It never initiates a second panel power transition.
    static void WakeScreen();
    static void CompleteAudioWake();
    static void CompletePeripheralWake(bool ready);
    static bool IsActive();
    static bool IsScreenOff();
    static uint32_t ScreenOffGeneration();
};

}  // namespace agent_ui
