#pragma once

#include <string>

#include "core/agent_ui_types.h"

namespace agent_ui::home {

struct ViewState {
    AgentState agent_state = AgentState::Idle;
    std::string message;
    bool conversation_message = false;
    bool user_message = false;

    bool operator==(const ViewState& other) const {
        return agent_state == other.agent_state && message == other.message &&
               conversation_message == other.conversation_message &&
               user_message == other.user_message;
    }

    bool operator!=(const ViewState& other) const { return !(*this == other); }
};

}  // namespace agent_ui::home
