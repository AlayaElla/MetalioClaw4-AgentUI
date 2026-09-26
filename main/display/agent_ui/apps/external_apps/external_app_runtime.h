#pragma once

#include <string>

#include "external_app_manager.h"
#include "lvgl.h"
#include "ai/ai_capabilities.h"

namespace agent_ui::external_apps {

class Runtime {
public:
    struct State;

    static Runtime& Get();

    bool Launch(const AppInfo& app, lv_obj_t* content, lv_obj_t* actions,
                std::string* error = nullptr);
    void SetPaused(bool paused);
    void Unload();
    // Manifest discovery is independent of whether an ELF is currently loaded.
    static void RegisterInstalledCapabilities(const std::vector<AppInfo>& apps);

private:
    Runtime() = default;
    ~Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    State* state_ = nullptr;
    uint64_t generation_ = 0;
    ai::OperationResult InvokeAction(const AppInfo& app, const ai::InvokeRequest& request);
};

}  // namespace agent_ui::external_apps
