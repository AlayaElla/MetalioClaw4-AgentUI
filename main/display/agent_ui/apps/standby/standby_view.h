#pragma once

#include "lvgl.h"

namespace agent_ui {

class StandbyView {
public:
    static void Show();
    static void HandlePowerKey();
    static bool IsActive();
    static bool IsScreenOff();
};

}  // namespace agent_ui
