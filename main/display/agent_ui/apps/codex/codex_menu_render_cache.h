#pragma once

#include <array>
#include "lvgl.h"

namespace agent_ui::codex_menu_render_cache {

struct Stats {
    uint32_t render_requests = 0;
    uint32_t cached_draws = 0;
    uint32_t buffer_flushes = 0;
    uint32_t snapshot_builds = 0;
    uint32_t row_refreshes = 0;
};

void Attach(lv_obj_t* task_list, lv_obj_t* task_panel, lv_obj_t* overlay,
            const std::array<lv_obj_t*, 6>& rows);
void SetVisible(lv_obj_t* content, bool visible);
void Invalidate(lv_obj_t* content);
void InvalidateRow(lv_obj_t* content, int row);
Stats GetStats(lv_obj_t* content);

}  // namespace agent_ui::codex_menu_render_cache
