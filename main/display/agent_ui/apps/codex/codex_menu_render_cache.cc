#include "codex_menu_render_cache.h"

#include <cstdint>
#include <new>
#include <array>

#include "core/theme.h"
#include "components/render_snapshot_buffer.h"

namespace agent_ui::codex_menu_render_cache {
namespace {

struct Cache {
    lv_obj_t* content = nullptr;
    lv_obj_t* task_panel = nullptr;
    lv_obj_t* overlay = nullptr;
    std::array<lv_obj_t*, 6> rows{};
    RenderSnapshotBuffer snapshot;
    uint32_t width = 0;
    uint32_t height = 0;
    lv_color_format_t format = LV_COLOR_FORMAT_UNKNOWN;
    bool visible = false;
    bool dirty = true;
    bool building = false;
    bool rebuild_queued = false;
    uint32_t observed_state = 0;
    bool observed_state_valid = false;
    uint32_t snapshot_state = 0;
    bool snapshot_state_valid = false;
    bool waiting_for_transient = false;
    Stats stats{};
    uint32_t theme[11]{};
    bool failed = false;
};

Cache* Get(lv_obj_t* content) {
    return content == nullptr ? nullptr : static_cast<Cache*>(lv_obj_get_user_data(content));
}

void ReadTheme(uint32_t (&colors)[11]) {
    const auto& c = Theme::Get().colors();
    colors[0] = c.background;
    colors[1] = c.surface;
    colors[2] = c.raised;
    colors[3] = c.border;
    colors[4] = c.text;
    colors[5] = c.muted;
    colors[6] = c.accent;
    colors[7] = c.accent_pressed;
    colors[8] = c.accent_ink;
    colors[9] = c.danger;
    colors[10] = c.warning;
}

void Release(Cache* cache) {
    if (cache == nullptr) return;
    cache->snapshot.Release();
    cache->width = cache->height = 0;
    cache->format = LV_COLOR_FORMAT_UNKNOWN;
}

void Schedule(Cache* cache);
void Rebuild(void* data);

void CancelRebuild(Cache* cache) {
    if (cache->rebuild_queued) {
        lv_async_call_cancel(Rebuild, cache);
        cache->rebuild_queued = false;
    }
}

uint32_t StateSignature(lv_obj_t* obj, uint32_t signature = 2166136261u) {
    constexpr lv_state_t kTransient = static_cast<lv_state_t>(LV_STATE_PRESSED | LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    signature = (signature ^ static_cast<uint32_t>(lv_obj_get_state(obj) & kTransient)) * 16777619u;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        signature = StateSignature(lv_obj_get_child(obj, static_cast<int32_t>(i)), signature);
    }
    return signature;
}

bool HasTransientState(lv_obj_t* obj) {
    constexpr lv_state_t kTransient = static_cast<lv_state_t>(LV_STATE_PRESSED | LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    if (lv_obj_has_state(obj, kTransient)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        if (HasTransientState(lv_obj_get_child(obj, static_cast<int32_t>(i)))) return true;
    }
    return false;
}

void Draw(lv_event_t* event) {
    auto* cache = static_cast<Cache*>(lv_event_get_user_data(event));
    if (cache == nullptr || cache->building || !cache->visible) return;
    ++cache->stats.render_requests;

    uint32_t current_theme[11]{};
    ReadTheme(current_theme);
    bool theme_changed = false;
    for (size_t i = 0; i < 11; ++i) theme_changed |= current_theme[i] != cache->theme[i];
    if (theme_changed) {
        cache->dirty = true;
        cache->failed = false;
        for (size_t i = 0; i < 11; ++i) cache->theme[i] = current_theme[i];
    }
    const uint32_t state_signature = StateSignature(cache->content);
    const bool transient = HasTransientState(cache->content);
    if (!cache->observed_state_valid || state_signature != cache->observed_state) {
        cache->observed_state = state_signature;
        cache->observed_state_valid = true;
        if (!transient && cache->snapshot_state_valid && state_signature != cache->snapshot_state) {
            cache->dirty = true;
            cache->failed = false;
        }
    }
    if (transient && (!cache->snapshot_state_valid || state_signature != cache->snapshot_state)) {
        cache->waiting_for_transient = true;
        return;
    }
    if (cache->waiting_for_transient) cache->waiting_for_transient = false;
    if (cache->dirty) {
        if (!cache->failed) Schedule(cache);
        return;
    }
    if (lv_obj_has_flag(cache->task_panel, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t area{};
    lv_obj_get_coords(cache->content, &area);
    if (static_cast<uint32_t>(lv_area_get_width(&area)) != cache->width ||
        static_cast<uint32_t>(lv_area_get_height(&area)) != cache->height) {
        cache->dirty = true;
        cache->failed = false;
        Schedule(cache);
        return;
    }

    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.src = cache->snapshot.buffer();
    lv_draw_image(lv_event_get_layer(event), &image, &area);
    ++cache->stats.cached_draws;
    // LVGL 9.3 checks layer->opa after DRAW_MAIN before traversing children.
    // refr_obj restores the incoming opacity when this object's draw returns.
    lv_event_get_layer(event)->opa = LV_OPA_TRANSP;
    lv_event_stop_processing(event);
}

bool EnsureBuffer(Cache* cache, uint32_t width, uint32_t height,
                  lv_color_format_t format) {
    return cache->snapshot.Prepare(width, height, format);
}

void Rebuild(void* data) {
    auto* cache = static_cast<Cache*>(data);
    if (cache == nullptr) return;
    cache->rebuild_queued = false;
    if (!cache->visible || !cache->dirty || cache->failed || cache->content == nullptr ||
        lv_obj_has_flag(cache->task_panel, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_has_flag(cache->overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (HasTransientState(cache->content)) {
        cache->waiting_for_transient = true;
        return;
    }
#if LV_USE_SNAPSHOT
    lv_obj_update_layout(cache->content);
    const uint32_t width = static_cast<uint32_t>(lv_obj_get_width(cache->content));
    const uint32_t height = static_cast<uint32_t>(lv_obj_get_height(cache->content));
    const lv_color_format_t format = lv_display_get_color_format(lv_obj_get_display(cache->content));
    if (width == 0 || height == 0 || !EnsureBuffer(cache, width, height, format)) {
        cache->failed = true;
        return;
    }

    cache->building = true;
    lv_obj_set_style_bg_color(cache->content,
        lv_color_hex(Theme::Get().colors().background), LV_PART_MAIN);
    const bool captured = cache->snapshot.Capture(cache->content);
    cache->building = false;
    if (!captured) {
        cache->failed = true;
        return;
    }

    ++cache->stats.buffer_flushes;
    ++cache->stats.snapshot_builds;
    cache->width = width;
    cache->height = height;
    cache->format = format;
    ReadTheme(cache->theme);
    cache->observed_state = StateSignature(cache->content);
    cache->observed_state_valid = true;
    cache->snapshot_state = cache->observed_state;
    cache->snapshot_state_valid = true;
    cache->dirty = false;
    cache->snapshot.RegisterOpaque("MenuPPA");
    lv_obj_invalidate(cache->content);
#endif
#if !LV_USE_SNAPSHOT
    cache->failed = true;
#endif
}

void Schedule(Cache* cache) {
    if (cache == nullptr || cache->rebuild_queued || !cache->visible) return;
    cache->rebuild_queued = true;
    if (lv_async_call(Rebuild, cache) != LV_RESULT_OK) cache->rebuild_queued = false;
}

void OnContentEvent(lv_event_t* event) {
    auto* cache = static_cast<Cache*>(lv_event_get_user_data(event));
    if (cache == nullptr) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        CancelRebuild(cache);
        Release(cache);
        delete cache;
        return;
    }
    if (cache->building) return;
    if (code == LV_EVENT_SIZE_CHANGED ||
        code == LV_EVENT_STYLE_CHANGED) {
        cache->dirty = true;
        cache->failed = false;
        Schedule(cache);
    }
}

}  // namespace

void Attach(lv_obj_t* content, lv_obj_t* task_panel, lv_obj_t* overlay,
            const std::array<lv_obj_t*, 6>& rows) {
    if (content == nullptr || Get(content) != nullptr) return;
    auto* cache = new (std::nothrow) Cache{};
    if (cache == nullptr) return;
    cache->content = content;
    cache->task_panel = task_panel;
    cache->overlay = overlay;
    cache->rows = rows;
    lv_obj_set_style_bg_color(content, lv_color_hex(Theme::Get().colors().background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, LV_PART_MAIN);
    ReadTheme(cache->theme);
    lv_obj_set_user_data(content, cache);
    lv_obj_add_event_cb(content, Draw, LV_EVENT_DRAW_MAIN, cache);
    lv_obj_add_event_cb(content, OnContentEvent, LV_EVENT_ALL, cache);
}

void SetVisible(lv_obj_t* content, bool visible) {
    Cache* cache = Get(content);
    if (cache == nullptr) return;
    if (!visible) {
        CancelRebuild(cache);
        cache->visible = false;
        cache->dirty = true;
        cache->failed = false;
        Release(cache);
        return;
    }
    cache->visible = visible;
    cache->dirty = true;
    cache->failed = false;
    if (visible) Schedule(cache);
}

void Invalidate(lv_obj_t* content) {
    Cache* cache = Get(content);
    if (cache == nullptr) return;
    cache->dirty = true;
    cache->failed = false;
    Schedule(cache);
}

void InvalidateRow(lv_obj_t* content, int row) {
    Cache* cache = Get(content);
    if (cache == nullptr || row < 0 || row >= static_cast<int>(cache->rows.size())) return;
    cache->dirty = true;
    cache->failed = false;
    ++cache->stats.row_refreshes;
    Schedule(cache);
}

Stats GetStats(lv_obj_t* content) {
    const Cache* cache = Get(content);
    return cache == nullptr ? Stats{} : cache->stats;
}

}  // namespace agent_ui::codex_menu_render_cache
