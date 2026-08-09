#include "home_module.h"

#include <utility>

#include "core/status_bar.h"

namespace agent_ui::home {

void Module::Initialize(const char* initial_message, NavigationSink navigation_sink) {
    navigation_sink_ = std::move(navigation_sink);
    controller_.HandleEvent(Event::GreetingMessage(initial_message));
    controller_.Activate(
        [this](const ViewState& state) { view_.Render(state); },
        [this](const Command& command) { HandleCommand(command); });
}

void Module::HandleEvent(const Event& event) {
    controller_.HandleEvent(event);
}

lv_obj_t* Module::Mount() {
    lv_obj_t* root =
        view_.Mount([this](const Intent& intent) { controller_.HandleIntent(intent); });
    view_.Render(controller_.state());
    return root;
}

void Module::Unmount() {
    view_.Unmount();
}

void Module::HandleCommand(const Command& command) {
    if (command.type == CommandType::OpenApp) {
        if (navigation_sink_) navigation_sink_(command.target);
        return;
    }
    StatusBar::Get().SetAgentState(controller_.state().agent_state);
    adapter_.Execute(command);
}

}  // namespace agent_ui::home
