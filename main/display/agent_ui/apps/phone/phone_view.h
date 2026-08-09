#pragma once

#include "lvgl.h"
#include "core/ui_utils.h"

namespace agent_ui {

class PhoneView {
public:
    // Create a 720x720 fullscreen phone-dialer page.
    // Returns a new LVGL screen object (parent = NULL); the caller is
    // responsible for loading it via lv_screen_load().
    // The page contains a dial keypad (1-9, 0 centered) with a larger
    // call/hangup button at the bottom-right, a number display, a backspace
    // button, and a back button that returns through Agent Navigation.
    static lv_obj_t* Create();

    // 进入 / 退出拨号界面时切换 IOExpander::PA_SWITCH：
    //   LOAD   -> PA_SWITCH=false（音频功放切到 4G 通话路径）
    //   UNLOAD -> PA_SWITCH=true （恢复 WIFI/本地音频路径）
    static void LifecycleCallback(AppLifecycleEvent event);
};

}  // namespace agent_ui
