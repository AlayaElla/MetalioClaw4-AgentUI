#pragma once

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace agent_ui {

class BootView {
public:
    static lv_obj_t* Create();
    static void SetInstallProgress(const char* app_name, size_t package_index,
                                   size_t package_count, uint8_t percent);
    static void SetReady();
};

}  // namespace agent_ui
