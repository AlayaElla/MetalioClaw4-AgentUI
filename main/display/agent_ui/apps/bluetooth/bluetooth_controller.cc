#include "bluetooth_controller.h"

#include <utility>

namespace agent_ui::bluetooth {

void Controller::Activate(StateSink state_sink, CommandSink command_sink) {
    state_sink_ = std::move(state_sink);
    command_sink_ = std::move(command_sink);
    PublishState();
}

void Controller::HandleIntent(const Intent& intent) {
    switch (intent.type) {
        case IntentType::SetEnabled:
            Dispatch({.type = CommandType::SetEnabled,
                      .enabled = intent.enabled});
            break;
        case IntentType::Scan:
            Dispatch({.type = CommandType::Scan});
            break;
        case IntentType::Connect:
            Dispatch({.type = CommandType::Connect, .index = intent.index});
            break;
        case IntentType::Reset:
            Dispatch({.type = CommandType::Reset});
            break;
        case IntentType::SetAudioProfile:
            Dispatch({.type = CommandType::SetAudioProfile,
                      .audio_profile = intent.audio_profile});
            break;
    }
}

void Controller::HandleEvent(const Event& event) {
    if (event.type != EventType::Snapshot) return;
    ViewState next = state_;
    next.enabled = event.enabled;
    next.scanning = event.scanning;
    next.resetting = event.resetting;
    next.connection = event.connection;
    next.audio_profile = event.audio_profile;
    next.has_current_device = event.has_current_device;
    next.current_device = event.current_device;
    next.nearby_devices = event.nearby_devices;
    if (next == state_) return;
    state_ = std::move(next);
    PublishState();
}

void Controller::HandleLifecycle(Lifecycle lifecycle) {
    switch (lifecycle) {
        case Lifecycle::Load:
        case Lifecycle::Resume:
            state_.mounted = true;
            Dispatch({.type = CommandType::Start});
            break;
        case Lifecycle::Suspend:
            state_.mounted = false;
            Dispatch({.type = CommandType::Stop});
            break;
        case Lifecycle::Unload:
            state_.mounted = false;
            Dispatch({.type = CommandType::Stop});
            break;
    }
    PublishState();
}

void Controller::Dispatch(const Command& command) const {
    if (command_sink_) command_sink_(command);
}

void Controller::PublishState() const {
    if (state_sink_) state_sink_(state_);
}

}  // namespace agent_ui::bluetooth
