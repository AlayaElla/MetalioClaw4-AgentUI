#include "network_module.h"

#include <utility>

namespace agent_ui::network {
namespace {

struct PendingEvent {
    Module* module = nullptr;
    Event event;
};

Lifecycle ToLifecycle(AppLifecycleEvent event) {
    switch (event) {
        case AppLifecycleEvent::Load:
            return Lifecycle::Load;
        case AppLifecycleEvent::Unload:
            return Lifecycle::Unload;
        case AppLifecycleEvent::Suspend:
            return Lifecycle::Suspend;
        case AppLifecycleEvent::Resume:
            return Lifecycle::Resume;
    }
    return Lifecycle::Unload;
}

}  // namespace

Module::Module() {
    adapter_.SetEventSink([this](const Event& event) { PostEvent(event); });
    controller_.Activate(
        [this](const ViewState& state) { ++revision_; view_.Render(state); },
        [this](const Command& command) { HandleCommand(command); });
}

bool Module::SubmitIntent(const Intent& intent) {
    if (!controller_.state().mounted) return false;
    controller_.HandleIntent(intent);
    return true;
}

bool Module::SetMode(int mode) {
    if (!controller_.state().mounted || mode < 0 || mode > 2) return false;
    if (mode == 0) {
        if (controller_.state().cellular) HandleCommand({.type = CommandType::SwitchNetwork});
    } else HandleCommand({.type = CommandType::SwitchSimSlot, .value = mode == 1 ? 1 : 0});
    return true;
}

void Module::BuildInto(lv_obj_t* parent) {
    view_.BuildInto(parent,
                    [this](const Intent& intent) { controller_.HandleIntent(intent); });
    view_.Render(controller_.state());
}

void Module::ResetUi() {
    view_.Reset();
}

void Module::SetConfigChangedCallback(Adapter::ConfigChangedSink callback) {
    adapter_.SetConfigChangedSink(std::move(callback));
}

void Module::LifecycleCallback(AppLifecycleEvent event) {
    ++session_;
    const Lifecycle lifecycle = ToLifecycle(event);
    if (lifecycle == Lifecycle::Unload) {
        controller_.HandleLifecycle(lifecycle);
        return;
    }
    view_.LifecycleCallback(lifecycle);
    controller_.HandleLifecycle(lifecycle);
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
    if (pending->module != nullptr) {
        if (pending->module->state().mounted) {
            ++pending->module->event_revisions_[static_cast<size_t>(pending->event.type)];
            if (pending->event.type == EventType::ScanFinished)
                pending->module->scan_succeeded_ = pending->event.success;
        }
        pending->module->controller_.HandleEvent(pending->event);
    }
    delete pending;
}

}  // namespace agent_ui::network
