#pragma once

#include <cstddef>

#include "lvgl.h"

namespace agent_ui {

void InitializeExpressionAcceleration();
void RegisterExpressionA8Buffer(const void* buffer, size_t size);
void UnregisterExpressionA8Buffer(const void* buffer);
// Only the keyboard's opaque, untransformed, non-tiled snapshot is eligible.
// Unregister before replacing its pixels or freeing its allocation.
void RegisterKeyboardRenderBuffer(const lv_draw_buf_t* buffer);
void UnregisterKeyboardRenderBuffer(const lv_draw_buf_t* buffer);

}  // namespace agent_ui
