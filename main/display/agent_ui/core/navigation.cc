#include "navigation.h"

#include "application.h"
#include "status_bar.h"
#include "theme.h"

namespace agent_ui {

Navigation& Navigation::Get() {
    static Navigation instance;
    return instance;
}

void Navigation::Register(ScreenId id, AppFactory factory) {
    factories_[static_cast<size_t>(id)] = factory;
}

void Navigation::Start() {
    stack_size_ = 0;
    Load(ScreenId::Home, TransitionDirection::Replace, true);
}

void Navigation::Open(ScreenId id) {
    if (id == current_) return;
    if (current_ == ScreenId::Home && id != ScreenId::Home) {
        Application::GetInstance().ForceReturnToIdle();
    }
    Load(id, TransitionDirection::Forward, true);
}

void Navigation::Back() {
    if (stack_size_ <= 1) {
        if (current_ != ScreenId::Home) Load(ScreenId::Home, TransitionDirection::Back, true);
        return;
    }
    --stack_size_;
    const ScreenId target = stack_[stack_size_ - 1];
    Load(target, TransitionDirection::Back, false);
}

void Navigation::RebuildCurrent() {
    Load(current_, TransitionDirection::Replace, false);
}

void Navigation::Load(ScreenId id, TransitionDirection direction, bool update_stack) {
    const AppFactory factory = factories_[static_cast<size_t>(id)];
    if (factory == nullptr) return;

    lv_obj_t* root = factory();
    if (root == nullptr) return;

    if (update_stack) {
        if (id == ScreenId::Home) {
            stack_size_ = 1;
            stack_[0] = ScreenId::Home;
        } else if (stack_size_ < stack_.size()) {
            stack_[stack_size_++] = id;
        } else {
            stack_[stack_.size() - 1] = id;
        }
    }
    current_ = id;
    StatusBar::Get().SetHomeActive(id == ScreenId::Home);
    StatusBar::Get().SetVisible(true);

    lv_screen_load_anim_t animation = LV_SCR_LOAD_ANIM_NONE;
    if (direction == TransitionDirection::Forward) {
        animation = LV_SCR_LOAD_ANIM_MOVE_LEFT;
    } else if (direction == TransitionDirection::Back) {
        animation = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
    } else {
        animation = LV_SCR_LOAD_ANIM_FADE_IN;
    }
    lv_screen_load_anim(root, animation, metrics::kTransitionMs, 0, true);
}

}  // namespace agent_ui
