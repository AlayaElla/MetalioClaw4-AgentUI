#pragma once

#include "agent_ui_types.h"
#include "lvgl.h"

namespace agent_ui {

class AppModule {
public:
    virtual ~AppModule() = default;

    virtual ScreenId screen_id() const = 0;
    virtual lv_obj_t* Mount() = 0;
    virtual void Unmount() = 0;
};

}  // namespace agent_ui
