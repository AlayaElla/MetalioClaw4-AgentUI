#include "home_adapter.h"

#include "application.h"
#include "assets/common_sounds.h"

namespace agent_ui::home {

void Adapter::Execute(const Command& command) {
    if (command.type == CommandType::ToggleListening) {
        Application::GetInstance().ToggleChatState();
    } else if (command.type == CommandType::PlayCarouselTick) {
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            // The detent is only UI feedback. Mixing it into the shared
            // Opus decode queue while TTS is streaming can reset the decoder
            // between different frame formats and stall speech.
            if (app.GetDeviceState() != kDeviceStateSpeaking) {
                app.PlaySound(CommonSounds::OGG_RATCHET_DETENT);
            }
        });
    }
}

}  // namespace agent_ui::home
