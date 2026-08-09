#include "network_controller.h"

#include <utility>

namespace agent_ui::network {

void Controller::Activate(StateSink state_sink, CommandSink command_sink) {
    state_sink_ = std::move(state_sink);
    command_sink_ = std::move(command_sink);
    PublishState();
}

void Controller::Deactivate() {
    state_sink_ = nullptr;
    command_sink_ = nullptr;
}

void Controller::Dispatch(const Command& command) const {
    if (command_sink_) command_sink_(command);
}

void Controller::HandleLifecycle(Lifecycle lifecycle) {
    switch (lifecycle) {
        case Lifecycle::Load:
        case Lifecycle::Resume:
            state_.mounted = true;
            Dispatch({.type = CommandType::Start});
            break;
        case Lifecycle::Unload:
        case Lifecycle::Suspend:
            state_.mounted = false;
            Dispatch({.type = CommandType::Stop});
            if (lifecycle == Lifecycle::Unload) {
                state_ = {};
            }
            break;
    }
    PublishState();
}

void Controller::HandleIntent(const Intent& intent) {
    switch (intent.type) {
        case IntentType::Scan:
            Dispatch({.type = CommandType::Scan});
            return;
        case IntentType::ConnectSaved:
            Dispatch({.type = CommandType::ConnectSaved, .index = intent.index});
            return;
        case IntentType::RequestPassword:
            state_.dialog = Dialog::Password;
            state_.dialog_ssid = intent.text;
            state_.dialog_title.clear();
            state_.dialog_detail.clear();
            PublishState();
            return;
        case IntentType::SubmitPassword:
            state_.dialog = Dialog::Connecting;
            state_.dialog_ssid = intent.text;
            state_.connecting = true;
            Dispatch({.type = CommandType::Connect,
                      .ssid = intent.text,
                      .password = intent.password});
            PublishState();
            return;
        case IntentType::SelectMode:
            if (intent.value >= 0 && intent.value <= 2) {
                state_.selected_mode = intent.value;
                PublishState();
            }
            return;
        case IntentType::SelectSimSlot:
            Dispatch({.type = CommandType::SwitchSimSlot, .value = intent.value});
            return;
        case IntentType::DismissStatus:
            state_.dialog = Dialog::None;
            state_.failure_auto_close_ms = 0;
            state_.dialog_title.clear();
            state_.dialog_detail.clear();
            PublishState();
            return;
    }
}

void Controller::HandleEvent(const Event& event) {
    if (!state_.mounted) return;
    ViewState next = state_;
    switch (event.type) {
        case EventType::ModeSnapshot:
            next.cellular = event.cellular;
            next.external_slot = event.external_slot;
            next.selected_mode = event.cellular ? (event.external_slot ? 2 : 1) : 0;
            break;
        case EventType::SavedNetworks:
            next.saved_networks = event.saved_networks;
            break;
        case EventType::NearbyNetworks:
            next.nearby_networks = event.nearby_networks;
            next.scanning = event.scanning;
            next.scan_started = event.scan_started;
            break;
        case EventType::Status:
            next.status = event.text;
            next.status_color = event.color;
            break;
        case EventType::ScanStarted:
            next.scan_started = true;
            next.scanning = true;
            break;
        case EventType::ScanFinished:
            next.scanning = false;
            next.scan_started = true;
            break;
        case EventType::ConnectStarted:
            next.connecting = true;
            next.dialog = Dialog::Connecting;
            next.dialog_ssid = event.ssid;
            break;
        case EventType::ConnectSucceeded:
            next.connecting = false;
            next.dialog = Dialog::Restart;
            next.dialog_title = event.text;
            next.dialog_detail.clear();
            next.restart_remaining = event.value;
            next.failure_auto_close_ms = 0;
            break;
        case EventType::ConnectFailed:
            next.connecting = false;
            next.network_switch_pending = false;
            next.sim_switch_pending = false;
            next.dialog = Dialog::Failure;
            next.dialog_title = event.text;
            next.dialog_detail = event.detail;
            next.failure_auto_close_ms = event.auto_close_ms;
            break;
        case EventType::NetworkSwitchStarted:
            next.network_switch_pending = true;
            next.dialog = Dialog::NetworkSwitch;
            next.dialog_title = event.text;
            break;
        case EventType::SimSwitchStarted:
            next.sim_switch_pending = true;
            next.dialog = Dialog::SimSwitch;
            next.dialog_title = event.text;
            break;
        case EventType::SimSwitchSucceeded:
            next.sim_switch_pending = false;
            next.dialog = Dialog::Restart;
            next.dialog_title = event.text;
            next.dialog_detail.clear();
            next.restart_remaining = event.value;
            next.failure_auto_close_ms = 0;
            break;
        case EventType::RestartCountdown:
            next.restart_remaining = event.value;
            break;
        case EventType::SimSlotSynced:
            next.cellular = true;
            next.external_slot = event.value == 0;
            next.selected_mode = next.external_slot ? 2 : 1;
            break;
    }

    if (next == state_) return;
    state_ = std::move(next);
    PublishState();
}

void Controller::PublishState() const {
    if (state_sink_) state_sink_(state_);
}

}  // namespace agent_ui::network
