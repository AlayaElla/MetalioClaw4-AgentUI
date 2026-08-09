#include "home_view.h"

#include <utility>

#include "application.h"
#include "agent_ui/apps/home/home_renderer.h"
#include "assets/common_sounds.h"

namespace agent_ui::home {

lv_obj_t* View::Mount(IntentSink intent_sink) {
    intent_sink_ = std::move(intent_sink);
    return Renderer::Create({
        .open_app =
            [this](ScreenId screen_id) {
                if (intent_sink_) intent_sink_(Intent::OpenApp(screen_id));
            },
        .toggle_listening =
            [this]() {
                if (intent_sink_) intent_sink_(Intent::ToggleListening());
            },
        .play_carousel_tick = []() {
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                // The detent is only UI feedback. Mixing it into the shared
                // Opus decode queue while TTS is streaming can reset the
                // decoder between different frame formats and stall speech.
                if (app.GetDeviceState() != kDeviceStateSpeaking) {
                    app.PlaySound(CommonSounds::OGG_RATCHET_DETENT);
                }
            });
        },
        .unmounted = [this]() { Unmount(); },
    });
}

void View::Render(const ViewState& state) {
    Renderer::RefreshAi(state.agent_state, state.message.c_str(),
                        state.conversation_message, state.user_message);
}

void View::Unmount() {
    intent_sink_ = nullptr;
}

}  // namespace agent_ui::home
