#include "home_adapter.h"

#include "application.h"

namespace agent_ui::home {

void Adapter::Execute(const Command& command) {
    if (command.type == CommandType::ToggleListening) {
        Application::GetInstance().ToggleChatState();
    }
}

}  // namespace agent_ui::home
