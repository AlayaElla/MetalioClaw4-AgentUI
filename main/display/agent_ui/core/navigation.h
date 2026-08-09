#pragma once

#include <array>
#include <cstddef>

#include "agent_ui_types.h"
#include "lvgl.h"

namespace agent_ui {

using AppFactory = lv_obj_t* (*)();

class Navigation {
public:
    static Navigation& Get();

    void Register(ScreenId id, AppFactory factory);
    void Start();
    void Open(ScreenId id);
    void Back();
    void RebuildCurrent();

    ScreenId current() const { return current_; }

private:
    Navigation() = default;
    void Load(ScreenId id, TransitionDirection direction, bool update_stack);

    static constexpr size_t kAppCount = static_cast<size_t>(ScreenId::DisplayDebug) + 1;
    std::array<AppFactory, kAppCount> factories_{};
    std::array<ScreenId, 8> stack_{};
    size_t stack_size_ = 0;
    ScreenId current_ = ScreenId::Home;
};

}  // namespace agent_ui
