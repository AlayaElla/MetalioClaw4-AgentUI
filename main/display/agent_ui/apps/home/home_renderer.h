#pragma once

#include <functional>

#include "core/agent_ui_types.h"
#include "lvgl.h"

namespace agent_ui::home {

struct RendererActions {
    std::function<void(ScreenId)> open_app;
    std::function<void()> toggle_listening;
    std::function<void()> play_carousel_tick;
    std::function<void()> unmounted;
};

class Renderer {
public:
    using WakeCompletedCallback = void (*)(void* user_data);

    static lv_obj_t* Create(RendererActions actions);
    static void RefreshAi(AgentState state, const char* message,
                          bool conversation_message, bool user_message);
    static void NotifyUserActivity();
    static void SleepExpression();
    static void EnterStandby();
    static void ExitStandby(WakeCompletedCallback callback = nullptr,
                            void* user_data = nullptr);
    static void SetRenderingPaused(bool paused);
    static lv_obj_t* Screen();
    static void UpdateBattery(bool has_battery, int level, bool charging);
    static void PlayDizzy();
    static void HoldChargingExpression();
    static void HoldDizzyExpression();
    static void ReleaseSpecialExpression();
    static bool IsMounted();
};

}  // namespace agent_ui::home
