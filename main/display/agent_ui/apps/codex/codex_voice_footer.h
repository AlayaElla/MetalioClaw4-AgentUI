#pragma once

#include <cstdint>

#include "lvgl.h"
#include "components/ui_components.h"

namespace agent_ui::codex_voice_footer {

struct Callbacks {
    lv_event_cb_t voice_pressed = nullptr;
    lv_event_cb_t voice_pressing = nullptr;
    lv_event_cb_t voice_released = nullptr;
    lv_event_cb_t stop_pressed = nullptr;
    lv_event_cb_t stop_released = nullptr;
    lv_event_cb_t open_menu = nullptr;
};

struct Palette {
    uint32_t danger = 0;
    uint32_t accent_ink = 0;
    uint32_t menu_glyph = 0;
};

struct Parts {
    ui_components::VoiceButtonParts voice{};
    ui_components::ActionButtonParts stop{};
    ui_components::ActionButtonParts menu{};
    lv_obj_t* stop_hold_progress = nullptr;
};

// Build the Codex bottom actions in an existing action bar. Behavior is supplied
// by the caller so this production layout can be exercised by the LVGL host test.
Parts Build(lv_obj_t* actions, const Palette& palette, const Callbacks& callbacks);

}  // namespace agent_ui::codex_voice_footer
