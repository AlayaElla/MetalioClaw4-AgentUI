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
// Register an opaque, packed RGB565/RGB888 snapshot for untransformed SRM copies.
// The caller guarantees draws are untiled and untransformed, and unregisters
// before changing pixels or freeing the buffer. The tag is copied into bounded
// internal storage and need not outlive this call.
void RegisterOpaqueRenderBuffer(const lv_draw_buf_t* buffer, const char* log_tag);
void UnregisterOpaqueRenderBuffer(const lv_draw_buf_t* buffer);

}  // namespace agent_ui
