#include "bluetooth_module.h"

namespace agent_ui::bluetooth {
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

Module::Module() : adapter_(Adapter::Get()) {
    controller_.Activate(
        [this](const ViewState& state) { view_.Render(state); },
        [this](const Command& command) { HandleCommand(command); });
}

void Module::InitializeHardware() {
    Adapter::Get().Initialize();
}

void Module::BuildInto(lv_obj_t* parent) {
    view_.BuildInto(parent, [this](const Intent& intent) {
        controller_.HandleIntent(intent);
    });
    view_.Render(controller_.state());
}

void Module::ResetUi() {
    view_.Reset();
}

void Module::LifecycleCallback(AppLifecycleEvent event) {
    const Lifecycle lifecycle = ToLifecycle(event);
    if (lifecycle == Lifecycle::Load || lifecycle == Lifecycle::Resume) {
        adapter_.SetEventSink(
            [this](const Event& adapter_event) { PostEvent(adapter_event); });
    }
    view_.LifecycleCallback(lifecycle);
    controller_.HandleLifecycle(lifecycle);
    if (lifecycle == Lifecycle::Unload || lifecycle == Lifecycle::Suspend) {
        adapter_.SetEventSink(nullptr);
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
    if (pending->module != nullptr) {
        pending->module->controller_.HandleEvent(pending->event);
    }
    delete pending;
}

}  // namespace agent_ui::bluetooth
