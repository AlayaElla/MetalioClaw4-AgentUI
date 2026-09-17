#pragma once
#include "lvgl.h"

namespace agent_ui::codex_status_ring {
lv_obj_t* Create(lv_obj_t* parent);
void Update(lv_obj_t* root, bool visible, bool animated, uint32_t color);
void Raise(lv_obj_t* root);
}  // namespace agent_ui::codex_status_ring
