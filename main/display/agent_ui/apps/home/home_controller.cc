#include "home_controller.h"

#include <utility>

namespace agent_ui::home {
namespace {

bool IsConversationRole(const std::string& role) {
    return role == "user" || role == "assistant";
}

}  // namespace

void Controller::Activate(StateSink state_sink, CommandSink command_sink) {
    state_sink_ = std::move(state_sink);
    command_sink_ = std::move(command_sink);
    PublishState();
}

void Controller::Deactivate() {
    state_sink_ = nullptr;
    command_sink_ = nullptr;
}

void Controller::HandleIntent(const Intent& intent) {
    if (!command_sink_) return;

    if (intent.type == IntentType::OpenApp) {
        command_sink_({.type = CommandType::OpenApp, .target = intent.target});
        return;
    }

    if (state_.agent_state == AgentState::Idle) {
        state_.agent_state = AgentState::Connecting;
        PublishState();
    }
    command_sink_({.type = CommandType::ToggleListening, .target = ScreenId::Home});
}

void Controller::HandleEvent(const Event& event) {
    ViewState next = state_;
    if (event.type == EventType::AgentStateChanged) {
        next.agent_state = event.agent_state;
    } else if (event.type == EventType::GreetingMessage) {
        if (event.content.empty()) return;
        next.message = event.content;
        next.conversation_message = false;
        next.user_message = false;
    } else {
        if (!IsConversationRole(event.role) || event.content.empty()) return;
        next.message = event.content;
        next.conversation_message = true;
        next.user_message = event.role == "user";
    }

    if (next == state_) return;
    state_ = std::move(next);
    PublishState();
}

void Controller::PublishState() const {
    if (state_sink_) state_sink_(state_);
}

}  // namespace agent_ui::home
