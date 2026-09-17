#include "codex_status_ring.h"
#include <algorithm>
#include <array>
#include "core/theme.h"

namespace agent_ui::codex_status_ring {
namespace {
constexpr int kSegments = 4, kRunLength = 480, kThickness = 10, kInset = 4;
struct Ring {
    lv_obj_t* root;
    std::array<lv_obj_t*, kSegments> trail{};
    lv_timer_t* timer = nullptr;
    uint32_t started = 0;
    uint32_t color = 0;
    bool visible = false, animated = false;
};
void Tick(lv_timer_t* timer) {
    auto* ring = static_cast<Ring*>(lv_timer_get_user_data(timer));
    const int w = lv_obj_get_width(ring->root) - kThickness;
    const int h = lv_obj_get_height(ring->root) - kThickness;
    const int perimeter = 2 * (w + h);
    if (w < kRunLength || h < kRunLength) return;
    const int head = static_cast<int>((lv_tick_elaps(ring->started) % 3600) * perimeter / 3600);
    const int start = (head + perimeter - kRunLength) % perimeter;
    const int end = start + kRunLength;
    const int corners[] = {0, w, w + h, 2 * w + h, perimeter};
    for (int i = 0; i < kSegments; ++i) {
        int from = std::max(start, corners[i]);
        int to = std::min(end, corners[i + 1]);
        if (from >= to && end > perimeter) { from = corners[i]; to = std::min(end - perimeter, corners[i + 1]); }
        if (from >= to) { lv_obj_add_flag(ring->trail[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(ring->trail[i], LV_OBJ_FLAG_HIDDEN);
        const int length = to - from + kThickness;
        int x = 0, y = 0, width = kThickness, height = kThickness;
        if (i == 0) { x = from; width = length; }
        else if (i == 1) { x = w; y = from - w; height = length; }
        else if (i == 2) { x = 2 * w + h - to; y = h; width = length; }
        else { y = perimeter - to; height = length; }
        lv_obj_set_pos(ring->trail[i], x, y);
        lv_obj_set_size(ring->trail[i], width, height);
    }
}
void OnDelete(lv_event_t* event) {
    auto* ring = static_cast<Ring*>(lv_event_get_user_data(event));
    if (ring->timer) lv_timer_delete(ring->timer);
    delete ring;
}
}
lv_obj_t* Create(lv_obj_t* parent) {
    auto* ring = new Ring{};
    ring->root = lv_obj_create(parent);
    lv_obj_remove_style_all(ring->root);
    lv_obj_add_flag(ring->root, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(ring->root, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(ring->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ring->root, metrics::kDisplaySize - 2 * kInset,
                    metrics::kDisplaySize - metrics::kStatusBarHeight - 2 * kInset);
    lv_obj_set_pos(ring->root, kInset, metrics::kStatusBarHeight + kInset);
    lv_obj_set_style_border_opa(ring->root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_user_data(ring->root, ring);
    lv_obj_add_event_cb(ring->root, OnDelete, LV_EVENT_DELETE, ring);
    for (int i = 0; i < kSegments; ++i) {
        auto* segment = lv_obj_create(ring->root);
        lv_obj_remove_style_all(segment);
        lv_obj_remove_flag(segment, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(segment, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, LV_PART_MAIN);
        ring->trail[i] = segment;
    }
    lv_obj_add_flag(ring->root, LV_OBJ_FLAG_HIDDEN);
    return ring->root;
}
void Raise(lv_obj_t* root) { if (root) lv_obj_move_foreground(root); }
void Update(lv_obj_t* root, bool visible, bool animated, uint32_t color) {
    if (!root) return;
    auto* ring = static_cast<Ring*>(lv_obj_get_user_data(root));
    if (ring->visible == visible && ring->animated == animated && ring->color == color) { Raise(root); return; }
    ring->visible = visible; ring->animated = animated; ring->color = color;
    if (visible) { lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN); Raise(root); }
    else lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(root, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_width(root, animated ? 0 : kThickness, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, animated ? 0 : -kThickness, LV_PART_MAIN);
    for (auto* segment : ring->trail) {
        lv_obj_set_style_bg_color(segment, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_add_flag(segment, LV_OBJ_FLAG_HIDDEN);
    }
    if (visible && animated && !ring->timer) {
        ring->started = lv_tick_get();
        ring->timer = lv_timer_create(Tick, 33, ring);
        lv_obj_update_layout(root); Tick(ring->timer);
    } else if ((!visible || !animated) && ring->timer) { lv_timer_delete(ring->timer); ring->timer = nullptr; }
    else if (visible && animated && ring->timer) Tick(ring->timer);
}
}  // namespace agent_ui::codex_status_ring
