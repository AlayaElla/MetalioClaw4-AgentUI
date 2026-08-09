#pragma once

#include <cstdint>

#include "lvgl.h"

namespace agent_ui {

enum class HapticStrength : uint8_t {
    Light,
    Medium,
};

// Plays a short PWM-driven vibration without blocking the caller.
void PlayHaptic(HapticStrength strength);

// Adds medium feedback without replacing the button's existing callbacks.
void AttachButtonHaptic(lv_obj_t* button);

}  // namespace agent_ui
