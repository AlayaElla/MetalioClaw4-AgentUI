#pragma once

#include "lvgl.h"

namespace agent_ui::external_apps {

class HostView {
public:
    static lv_obj_t* Create();
};

}  // namespace agent_ui::external_apps
