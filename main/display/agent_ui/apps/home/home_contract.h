#pragma once

#include <string>

#include "core/agent_ui_types.h"

namespace agent_ui::home {

enum class IntentType {
    OpenApp,
    ToggleListening,
};

struct Intent {
    IntentType type = IntentType::ToggleListening;
    ScreenId target = ScreenId::Home;

    static Intent OpenApp(ScreenId target_screen) {
        return {.type = IntentType::OpenApp, .target = target_screen};
    }

    static Intent ToggleListening() {
        return {.type = IntentType::ToggleListening, .target = ScreenId::Home};
    }
};

enum class CommandType {
    OpenApp,
    ToggleListening,
};

struct Command {
    CommandType type = CommandType::ToggleListening;
    ScreenId target = ScreenId::Home;
};

enum class EventType {
    AgentStateChanged,
    GreetingMessage,
    ConversationMessage,
};

struct Event {
    EventType type = EventType::AgentStateChanged;
    AgentState agent_state = AgentState::Idle;
    std::string role;
    std::string content;

    static Event AgentStateChanged(AgentState state) {
        return {.type = EventType::AgentStateChanged, .agent_state = state};
    }

    static Event GreetingMessage(const char* content) {
        Event event;
        event.type = EventType::GreetingMessage;
        event.content = content != nullptr ? content : "";
        return event;
    }

    static Event ConversationMessage(const char* message_role, const char* message_content) {
        Event event;
        event.type = EventType::ConversationMessage;
        event.role = message_role != nullptr ? message_role : "";
        event.content = message_content != nullptr ? message_content : "";
        return event;
    }
};

}  // namespace agent_ui::home
