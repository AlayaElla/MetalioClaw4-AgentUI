#include "home_view.h"

#include <utility>

#include "agent_ui/apps/home/home_renderer.h"

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
        .play_carousel_tick = [this]() {
            if (intent_sink_) intent_sink_(Intent::PlayCarouselTick());
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

void View::NotifyUserActivity() { Renderer::NotifyUserActivity(); }

void View::SleepExpression() { Renderer::SleepExpression(); }

bool View::IsMounted() const { return Renderer::IsMounted(); }

void View::UpdateBattery(bool has_battery, int level, bool charging) {
    Renderer::UpdateBattery(has_battery, level, charging);
}

void View::PlayDizzy() { Renderer::PlayDizzy(); }

void View::HoldChargingExpression() { Renderer::HoldChargingExpression(); }

void View::HoldDizzyExpression() { Renderer::HoldDizzyExpression(); }

void View::ReleaseSpecialExpression() { Renderer::ReleaseSpecialExpression(); }

bool View::OwnsScreen(lv_obj_t* screen) const {
    return screen != nullptr && Renderer::Screen() == screen;
}

void View::SetRenderingPaused(bool paused) {
    Renderer::SetRenderingPaused(paused);
}

}  // namespace agent_ui::home
