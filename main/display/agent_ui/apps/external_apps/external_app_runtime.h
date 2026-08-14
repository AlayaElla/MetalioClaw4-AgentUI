#pragma once

#include <string>

#include "external_app_manager.h"
#include "lvgl.h"

namespace agent_ui::external_apps {

class Runtime {
public:
    struct State;

    static Runtime& Get();

    bool Launch(const AppInfo& app, lv_obj_t* content, lv_obj_t* actions,
                std::string* error = nullptr);
    void SetPaused(bool paused);
    void Unload();

private:
    Runtime() = default;
    ~Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    State* state_ = nullptr;
};

}  // namespace agent_ui::external_apps
