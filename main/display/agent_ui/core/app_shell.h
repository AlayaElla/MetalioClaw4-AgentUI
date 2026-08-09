#pragma once

#include "agent_ui_types.h"
#include "lvgl.h"

namespace agent_ui {

struct AppShell {
    lv_obj_t* root = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* actions = nullptr;
    lv_obj_t* back = nullptr;
};

AppShell CreateAppShell(const char* title, const char* subtitle,
                        bool show_back = true,
                        lv_event_cb_t back_callback = nullptr);
}  // namespace agent_ui
