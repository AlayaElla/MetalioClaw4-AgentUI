#pragma once

#include "lvgl_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"

namespace agent_ui::opaque_render_acceleration {

bool TryCopyRenderBuffer(lv_draw_task_t* task,
                         const lv_draw_sw_blend_dsc_t* descriptor,
                         lv_color_format_t format);
// The expression façade calls this before initializing the shared PPA blend
// client, preserving the prior no-side-effect behavior for invalid buffers.
bool IsRegistrationBufferSupported(const lv_draw_buf_t* buffer);
void RegisterKeyboardBuffer(const lv_draw_buf_t* buffer);
void UnregisterKeyboardBuffer(const lv_draw_buf_t* buffer);
void RegisterBuffer(const lv_draw_buf_t* buffer, const char* log_tag);
void UnregisterBuffer(const lv_draw_buf_t* buffer);

}  // namespace agent_ui::opaque_render_acceleration
