#include "codex_voice_footer.h"

#include "font_awesome.h"

namespace agent_ui::codex_voice_footer {
namespace {

void AddMenuGlyph(lv_obj_t* button, uint32_t color) {
    if (button == nullptr) return;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* line = lv_obj_create(button);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 28, 3);
        lv_obj_set_style_bg_color(line, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(line, 2, LV_PART_MAIN);
        lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 12 + i * 8);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
}

}  // namespace

Parts Build(lv_obj_t* actions, const Palette& palette, const Callbacks& callbacks) {
    Parts parts{};
    parts.voice = ui_components::AddBottomVoiceButton(actions, "按住说话", nullptr);
    parts.stop = ui_components::AddBottomPrimaryButton(
        actions, FONT_AWESOME_STOP, "停止", nullptr, nullptr);

    lv_obj_set_width(parts.voice.root, 0);
    lv_obj_set_width(parts.stop.root, 0);
    lv_obj_set_flex_grow(parts.voice.root, 2);
    lv_obj_set_flex_grow(parts.stop.root, 1);
    lv_obj_set_style_bg_color(parts.stop.root, lv_color_hex(palette.danger), LV_PART_MAIN);
    lv_obj_set_style_bg_color(parts.stop.root,
        lv_color_darken(lv_color_hex(palette.danger), LV_OPA_20), LV_STATE_PRESSED);
    lv_obj_set_style_opa(parts.stop.root, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_flag(parts.stop.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(parts.stop.root, callbacks.stop_pressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(parts.stop.root, callbacks.stop_released, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(parts.stop.root, callbacks.stop_released, LV_EVENT_PRESS_LOST, nullptr);

    parts.stop_hold_progress = lv_obj_create(parts.stop.root);
    lv_obj_remove_style_all(parts.stop_hold_progress);
    lv_obj_set_size(parts.stop_hold_progress, 0, 7);
    lv_obj_align(parts.stop_hold_progress, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(
        parts.stop_hold_progress, lv_color_hex(palette.accent_ink), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parts.stop_hold_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(parts.stop_hold_progress, 4, LV_PART_MAIN);
    lv_obj_remove_flag(parts.stop_hold_progress, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(parts.voice.root, callbacks.voice_pressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_flag(parts.voice.root, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(parts.voice.root, callbacks.voice_pressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(parts.voice.root, callbacks.voice_released, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(parts.voice.root, callbacks.voice_released, LV_EVENT_PRESS_LOST, nullptr);

    parts.menu = ui_components::AddBottomActionButton(
        actions, nullptr, "菜单", callbacks.open_menu);
    AddMenuGlyph(parts.menu.root, palette.menu_glyph);
    return parts;
}

}  // namespace agent_ui::codex_voice_footer
