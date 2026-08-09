#include "camera_module.h"

#include <utility>

#include "core/app_shell.h"
#include "core/navigation.h"

namespace agent_ui::camera {
namespace {

struct PendingEvent {
    Module* module = nullptr;
    Event event;
};

Module* s_active_module = nullptr;

}  // namespace

Module::Module() {
    adapter_.SetEventSink([this](const Event& event) { PostEvent(event); });
    controller_.Activate(
        [this](const ViewState& state) { view_.Render(state); },
        [this](const Command& command) { HandleCommand(command); });
}

lv_obj_t* Module::Mount() {
    auto shell = CreateAppShell("Camera", nullptr, false);
    // Camera owns a full-height content area and its own action bars.
    if (shell.content != nullptr) {
        lv_obj_add_flag(shell.content, LV_OBJ_FLAG_HIDDEN);
    }
    if (shell.actions != nullptr) {
        lv_obj_add_flag(shell.actions, LV_OBJ_FLAG_HIDDEN);
    }
    s_active_module = this;
    view_.BuildInto(shell.root, [this](const Intent& intent) {
        controller_.HandleIntent(intent);
        if (intent.type == IntentType::NavigateBack) {
            Navigation::Get().Back();
        }
    });
    AttachAppLifecycle(shell.root, LifecycleThunk);
    view_.Render(controller_.state());
    return shell.root;
}

void Module::ResetUi() {
    view_.Reset();
}

void Module::LifecycleCallback(AppLifecycleEvent event) {
    controller_.HandleLifecycle(event);
    if (event == AppLifecycleEvent::Unload) {
        view_.LifecycleCallback(Lifecycle::Unload);
        if (s_active_module == this) s_active_module = nullptr;
        return;
    }
    switch (event) {
        case AppLifecycleEvent::Load:
            view_.LifecycleCallback(Lifecycle::Load);
            break;
        case AppLifecycleEvent::Suspend:
            view_.LifecycleCallback(Lifecycle::Suspend);
            break;
        case AppLifecycleEvent::Resume:
            view_.LifecycleCallback(Lifecycle::Resume);
            break;
        case AppLifecycleEvent::Unload:
            break;
    }
}

void Module::HandleCommand(const Command& command) {
    adapter_.Execute(command);
}

void Module::PostEvent(const Event& event) {
    auto* pending = new PendingEvent{this, event};
    lv_async_call(ApplyEvent, pending);
}

void Module::ApplyEvent(void* data) {
    auto* pending = static_cast<PendingEvent*>(data);
    if (pending == nullptr) return;
    if (pending->module != nullptr && pending->module == s_active_module) {
        pending->module->controller_.HandleEvent(pending->event);
    }
    delete pending;
}

void Module::LifecycleThunk(AppLifecycleEvent event) {
    if (s_active_module != nullptr) s_active_module->LifecycleCallback(event);
}

}  // namespace agent_ui::camera
