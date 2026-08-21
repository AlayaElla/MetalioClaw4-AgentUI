#include "ui_components.h"

#include <font_awesome.h>
#include "lvgl_private.h"

#include "components/haptic_feedback.h"
#include "core/fonts.h"
#include "core/theme.h"
#include "core/ui_utils.h"

namespace agent_ui::ui_components {

namespace {

constexpr lv_style_selector_t Selector(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

constexpr int kBottomActionSideHitExpansion = 30;
constexpr int kBottomActionBottomHitExpansion = 38;
constexpr int kBottomPrimaryBarBottomPadding = 14;
constexpr lv_opa_t kAccentSoftOpacity = 0x1F;
constexpr int kVoiceSignalHeight = 44;
constexpr int kVoiceWaveHeights[kVoiceWaveCount] = {14, 27, 40, 34, 27, 14};
int s_leading_action_hit_tag;
int s_trailing_action_hit_tag;

void BottomActionHitTest(lv_event_t* event) {
    auto* info = lv_event_get_hit_test_info(event);
    lv_obj_t* button = lv_event_get_current_target_obj(event);
    if (info == nullptr || info->point == nullptr || button == nullptr) return;

    lv_area_t coords{};
    lv_obj_get_coords(button, &coords);
    const bool leading =
        lv_event_get_user_data(event) == &s_leading_action_hit_tag;
    const int left = leading ? coords.x1 - kBottomActionSideHitExpansion
                             : coords.x1;
    const int right = leading ? coords.x2
                              : coords.x2 + kBottomActionSideHitExpansion;
    info->res = info->point->x >= left && info->point->x <= right &&
                info->point->y >= coords.y1 &&
                info->point->y <= coords.y2 + kBottomActionBottomHitExpansion;
}

void AttachBottomActionHitArea(lv_obj_t* button, bool leading) {
    lv_obj_set_ext_click_area(button, kBottomActionBottomHitExpansion);
    lv_obj_add_flag(button, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(button, BottomActionHitTest, LV_EVENT_HIT_TEST,
                        leading ? static_cast<void*>(&s_leading_action_hit_tag)
                                : static_cast<void*>(&s_trailing_action_hit_tag));
}

lv_obj_t* CreateBottomPrimaryRoot(lv_obj_t* bar, lv_event_cb_t callback,
                                  void* user_data) {
    if (bar == nullptr) return nullptr;
    const auto& colors = Theme::Get().colors();
    lv_obj_set_style_pad_bottom(bar, kBottomPrimaryBarBottomPadding,
                                LV_PART_MAIN);

    lv_obj_t* root = CreateButton(bar);
    lv_obj_remove_style_all(root);
    lv_obj_set_height(root, metrics::kBottomPrimaryActionHeight);
    lv_obj_set_flex_grow(root, 1);
    lv_obj_set_style_bg_color(root, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(root, lv_color_hex(colors.accent_pressed),
                              Selector(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(root, metrics::kRadiusControl, LV_PART_MAIN);
    if (callback != nullptr) {
        lv_obj_add_event_cb(root, callback, LV_EVENT_CLICKED, user_data);
    }
    IgnoreSwipeBack(root, true);
    return root;
}

void SetCenteredVoiceWaveHeight(void* object, int32_t height) {
    auto* wave = static_cast<lv_obj_t*>(object);
    if (wave == nullptr || !lv_obj_is_valid(wave)) return;
    lv_obj_set_height(wave, height);
    lv_obj_set_y(wave, (kVoiceSignalHeight - height) / 2);
}

struct SliderGuard {
    int last_pressed_value = 0;
    bool pointer_down = false;
    bool restoring = false;
};

void GuardSliderRelease(lv_event_t* event) {
    auto* guard = static_cast<SliderGuard*>(lv_event_get_user_data(event));
    if (guard == nullptr) return;

    lv_obj_t* slider = lv_event_get_current_target_obj(event);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        delete guard;
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        guard->pointer_down = true;
        guard->last_pressed_value = lv_slider_get_value(slider);
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        if (guard->restoring) return;
        lv_indev_t* indev = lv_indev_active();
        const bool pointer_release =
            guard->pointer_down && indev != nullptr &&
            lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED;
        if (pointer_release) {
            guard->restoring = true;
            lv_slider_set_value(slider, guard->last_pressed_value, LV_ANIM_OFF);
            guard->restoring = false;
            return;
        }
        guard->last_pressed_value = lv_slider_get_value(slider);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        // Some touch controllers publish (0, 0) for the released sample. LVGL
        // recalculates the slider once more on RELEASED, which would turn that
        // sample into the minimum value. Restore the last value observed while
        // the pointer was still down, then commit it exactly once.
        guard->pointer_down = false;
        if (lv_slider_get_value(slider) != guard->last_pressed_value) {
            guard->restoring = true;
            lv_slider_set_value(slider, guard->last_pressed_value, LV_ANIM_OFF);
            guard->restoring = false;
        }
    }
}

void ToggleSwitchFromRow(lv_event_t* event) {
    lv_obj_t* row = lv_event_get_current_target_obj(event);
    // A click on the switch itself can bubble to the row. The switch has
    // already updated its state in that case, so only handle direct row taps.
    if (row == nullptr || lv_event_get_target_obj(event) != row) return;

    auto* control = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    if (control == nullptr || !lv_obj_is_valid(control) ||
        lv_obj_has_state(control, LV_STATE_DISABLED)) {
        return;
    }

    PlayHaptic(HapticStrength::Medium);
    if (lv_obj_has_state(control, LV_STATE_CHECKED)) {
        lv_obj_clear_state(control, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(control, LV_STATE_CHECKED);
    }
    lv_obj_send_event(control, LV_EVENT_VALUE_CHANGED, nullptr);
}

}  // namespace

lv_obj_t* CreateButton(lv_obj_t* parent) {
    lv_obj_t* button = lv_button_create(parent);
    AttachButtonHaptic(button);
    return button;
}

lv_obj_t* CreateModalOverlay(lv_obj_t* parent) {
    if (parent == nullptr) return nullptr;
    lv_obj_t* overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    IgnoreSwipeBack(overlay, true);
    return overlay;
}

lv_obj_t* CreateModalSurface(lv_obj_t* overlay, int width, int height) {
    if (overlay == nullptr) return nullptr;
    lv_obj_t* surface = lv_obj_create(overlay);
    StyleSurface(surface);
    lv_obj_set_size(surface, width, height);
    lv_obj_set_style_pad_all(surface, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(surface, 0, LV_PART_MAIN);
    return surface;
}

lv_obj_t* CreateBottomActionBar(lv_obj_t* parent, int y) {
    if (parent == nullptr) return nullptr;
    if (y < 0) y = metrics::kBottomActionBarY;
    const auto& colors = Theme::Get().colors();
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), metrics::kBottomActionBarHeight);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors.background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_pad_left(bar, kBottomActionSideHitExpansion, LV_PART_MAIN);
    lv_obj_set_style_pad_right(bar, kBottomActionSideHitExpansion, LV_PART_MAIN);
    lv_obj_set_style_pad_top(bar, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(bar, kBottomActionBottomHitExpansion, LV_PART_MAIN);
    lv_obj_set_style_pad_column(bar, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

ActionButtonParts AddBottomActionButton(
    lv_obj_t* bar, const char* icon, const char* label,
    lv_event_cb_t callback, void* user_data, bool danger) {
    ActionButtonParts parts{};
    if (bar == nullptr) return parts;
    const bool leading = lv_obj_get_child_count(bar) == 0;
    const auto& colors = Theme::Get().colors();
    parts.root = CreateButton(bar);
    lv_obj_remove_style_all(parts.root);
    lv_obj_set_size(parts.root, 72, metrics::kBottomActionHeight);
    lv_obj_set_style_bg_opa(parts.root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(parts.root, lv_color_hex(colors.raised),
                              Selector(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(parts.root, LV_OPA_COVER,
                            Selector(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(parts.root, metrics::kRadiusControl, LV_PART_MAIN);
    if (callback != nullptr) {
        lv_obj_add_event_cb(parts.root, callback, LV_EVENT_CLICKED, user_data);
    }
    IgnoreSwipeBack(parts.root, true);
    AttachBottomActionHitArea(parts.root, leading);

    parts.icon = lv_label_create(parts.root);
    lv_label_set_text(parts.icon, icon != nullptr ? icon : "");
    lv_obj_set_style_text_font(parts.icon, fonts::Icon(), LV_PART_MAIN);
    lv_obj_set_style_text_color(
        parts.icon, lv_color_hex(danger ? colors.danger : colors.muted), LV_PART_MAIN);
    lv_obj_align(parts.icon, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_remove_flag(parts.icon, LV_OBJ_FLAG_CLICKABLE);

    parts.label = lv_label_create(parts.root);
    lv_label_set_text(parts.label, label != nullptr ? label : "");
    lv_obj_set_style_text_font(parts.label, fonts::SmallBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(
        parts.label, lv_color_hex(danger ? colors.danger : colors.muted), LV_PART_MAIN);
    lv_obj_align(parts.label, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_remove_flag(parts.label, LV_OBJ_FLAG_CLICKABLE);
    return parts;
}

ActionButtonParts AddBottomPrimaryButton(
    lv_obj_t* bar, const char* icon, const char* label,
    lv_event_cb_t callback, void* user_data) {
    ActionButtonParts parts{};
    if (bar == nullptr) return parts;
    const auto& colors = Theme::Get().colors();
    parts.root = CreateBottomPrimaryRoot(bar, callback, user_data);

    lv_obj_t* content = lv_obj_create(parts.root);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(content, 14, LV_PART_MAIN);
    lv_obj_center(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);

    parts.icon = lv_label_create(content);
    lv_label_set_text(parts.icon, icon != nullptr ? icon : "");
    lv_obj_set_style_text_font(parts.icon, fonts::Icon(), LV_PART_MAIN);
    lv_obj_set_style_text_color(parts.icon, lv_color_hex(colors.accent_ink),
                                LV_PART_MAIN);
    parts.label = lv_label_create(content);
    lv_label_set_text(parts.label, label != nullptr ? label : "");
    lv_obj_set_style_text_font(parts.label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(parts.label, lv_color_hex(colors.accent_ink),
                                LV_PART_MAIN);
    return parts;
}

VoiceButtonParts AddBottomVoiceButton(
    lv_obj_t* bar, const char* label, lv_event_cb_t callback, void* user_data) {
    VoiceButtonParts parts{};
    if (bar == nullptr) return parts;
    const auto& colors = Theme::Get().colors();
    parts.root = CreateBottomPrimaryRoot(bar, callback, user_data);
    lv_obj_set_style_transform_scale_x(
        parts.root, 252, Selector(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_transform_scale_y(
        parts.root, 252, Selector(LV_PART_MAIN, LV_STATE_PRESSED));

    parts.icon = lv_label_create(parts.root);
    lv_label_set_text(parts.icon, FONT_AWESOME_MICROPHONE);
    lv_obj_set_style_text_font(parts.icon, fonts::IconLarge(), LV_PART_MAIN);
    lv_obj_set_style_text_color(parts.icon, lv_color_hex(colors.accent_ink),
                                LV_PART_MAIN);
    lv_obj_align(parts.icon, LV_ALIGN_LEFT_MID, 34, 0);
    lv_obj_remove_flag(parts.icon, LV_OBJ_FLAG_CLICKABLE);

    parts.label = lv_label_create(parts.root);
    lv_label_set_text(parts.label, label != nullptr ? label : "");
    lv_obj_set_style_text_font(parts.label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(parts.label, lv_color_hex(colors.accent_ink),
                                LV_PART_MAIN);
    lv_obj_center(parts.label);
    lv_obj_remove_flag(parts.label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* signal = lv_obj_create(parts.root);
    lv_obj_remove_style_all(signal);
    lv_obj_set_size(signal, 90, 44);
    lv_obj_align(signal, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_remove_flag(signal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(signal, LV_OBJ_FLAG_CLICKABLE);

    parts.record_dot = lv_obj_create(signal);
    lv_obj_remove_style_all(parts.record_dot);
    lv_obj_set_size(parts.record_dot, 12, 12);
    lv_obj_set_style_bg_color(parts.record_dot, lv_color_hex(0xFF3B30),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parts.record_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(parts.record_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_align(parts.record_dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_remove_flag(parts.record_dot, LV_OBJ_FLAG_CLICKABLE);

    for (size_t i = 0; i < kVoiceWaveCount; ++i) {
        lv_obj_t* wave = lv_obj_create(signal);
        parts.waves[i] = wave;
        lv_obj_remove_style_all(wave);
        lv_obj_set_size(wave, 4, kVoiceWaveHeights[i]);
        lv_obj_set_pos(wave, 32 + static_cast<int>(i) * 10,
                       (kVoiceSignalHeight - kVoiceWaveHeights[i]) / 2);
        lv_obj_set_style_bg_color(wave, lv_color_hex(colors.accent_ink),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(wave, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(wave, 2, LV_PART_MAIN);
        lv_obj_remove_flag(wave, LV_OBJ_FLAG_CLICKABLE);
    }
    return parts;
}

void SetVoiceButtonAnimating(const VoiceButtonParts& parts, bool active) {
    constexpr int kWaveMinHeights[kVoiceWaveCount] = {8, 12, 16, 10, 14, 8};
    constexpr int kWaveMaxHeights[kVoiceWaveCount] = {30, 42, 28, 40, 36, 32};
    constexpr uint32_t kWaveDurations[kVoiceWaveCount] = {
        250, 310, 220, 340, 280, 240,
    };
    constexpr uint32_t kWaveDelays[kVoiceWaveCount] = {
        0, 70, 130, 35, 105, 175,
    };

    for (size_t i = 0; i < kVoiceWaveCount; ++i) {
        lv_obj_t* wave = parts.waves[i];
        if (wave == nullptr || !lv_obj_is_valid(wave)) continue;
        lv_anim_delete(wave, SetCenteredVoiceWaveHeight);
        if (!active) {
            SetCenteredVoiceWaveHeight(wave, kVoiceWaveHeights[i]);
            continue;
        }

        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, wave);
        lv_anim_set_exec_cb(&animation, SetCenteredVoiceWaveHeight);
        lv_anim_set_values(&animation, kWaveMinHeights[i], kWaveMaxHeights[i]);
        lv_anim_set_duration(&animation, kWaveDurations[i]);
        lv_anim_set_reverse_duration(&animation, kWaveDurations[i]);
        lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&animation, kWaveDelays[i]);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
        lv_anim_start(&animation);
    }
}

lv_obj_t* AddBottomActionSpacer(lv_obj_t* bar) {
    if (bar == nullptr) return nullptr;
    lv_obj_t* spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, metrics::kBottomActionHeight);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    return spacer;
}

ActionButtonParts AddWideActionButton(
    lv_obj_t* parent, const char* icon, const char* label,
    lv_event_cb_t callback, void* user_data) {
    ActionButtonParts parts{};
    if (parent == nullptr) return parts;
    const auto& colors = Theme::Get().colors();
    parts.root = CreateButton(parent);
    lv_obj_remove_style_all(parts.root);
    lv_obj_set_size(parts.root, LV_PCT(100), 72);
    lv_obj_set_style_margin_top(parts.root, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(parts.root, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parts.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(parts.root, lv_color_hex(colors.accent_pressed),
                              Selector(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_opa(parts.root, LV_OPA_40,
                         Selector(LV_PART_MAIN, LV_STATE_DISABLED));
    lv_obj_set_style_border_color(parts.root, lv_color_hex(colors.accent),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(parts.root, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(parts.root, metrics::kRadiusControl, LV_PART_MAIN);
    if (callback != nullptr) {
        lv_obj_add_event_cb(parts.root, callback, LV_EVENT_CLICKED, user_data);
    }
    IgnoreSwipeBack(parts.root, true);

    lv_obj_t* content = lv_obj_create(parts.root);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(content, 14, LV_PART_MAIN);
    lv_obj_center(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);

    parts.icon = lv_label_create(content);
    lv_label_set_text(parts.icon, icon != nullptr ? icon : "");
    lv_obj_set_style_text_font(parts.icon, fonts::Icon(), LV_PART_MAIN);
    lv_obj_set_style_text_color(parts.icon, lv_color_hex(colors.accent_ink),
                                LV_PART_MAIN);
    parts.label = lv_label_create(content);
    lv_label_set_text(parts.label, label != nullptr ? label : "");
    lv_obj_set_style_text_font(parts.label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(parts.label, lv_color_hex(colors.accent_ink),
                                LV_PART_MAIN);
    return parts;
}

void StylePanel(lv_obj_t* panel) {
    if (panel == nullptr) return;
    const auto& colors = Theme::Get().colors();
    lv_obj_remove_style_all(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(colors.background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(panel, metrics::kPagePadding, LV_PART_MAIN);
    lv_obj_set_style_pad_top(panel, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(panel, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
}

lv_obj_t* CreateSectionHeading(lv_obj_t* parent, const char* title,
                               const char* subtitle) {
    lv_obj_t* heading = lv_obj_create(parent);
    lv_obj_remove_style_all(heading);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_obj_set_height(heading, subtitle != nullptr && subtitle[0] != '\0' ? 62 : 34);
    lv_obj_remove_flag(heading, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title_label = lv_label_create(heading);
    lv_label_set_text(title_label, title != nullptr ? title : "");
    lv_obj_set_style_text_font(title_label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label,
                                lv_color_hex(Theme::Get().colors().text), LV_PART_MAIN);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    if (subtitle != nullptr && subtitle[0] != '\0') {
        lv_obj_t* subtitle_label = lv_label_create(heading);
        lv_label_set_text(subtitle_label, subtitle);
        lv_obj_set_width(subtitle_label, LV_PCT(100));
        lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(subtitle_label, fonts::Small(), LV_PART_MAIN);
        lv_obj_set_style_text_color(subtitle_label,
                                    lv_color_hex(Theme::Get().colors().muted),
                                    LV_PART_MAIN);
        lv_obj_align(subtitle_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    return heading;
}

lv_obj_t* CreateRow(lv_obj_t* parent, const char* icon, const char* title,
                    const char* subtitle, int height, int title_width) {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(colors.accent_pressed),
                              Selector(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_color(row, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, metrics::kRadiusControl, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 18, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    int text_x = 0;
    if (icon != nullptr && icon[0] != '\0') {
        lv_obj_t* icon_label = lv_label_create(row);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, fonts::Icon(), LV_PART_MAIN);
        lv_obj_set_style_text_color(icon_label, lv_color_hex(colors.accent),
                                    LV_PART_MAIN);
        lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 0, 0);
        text_x = 54;
    }

    lv_obj_t* title_label = lv_label_create(row);
    lv_label_set_text(title_label, title != nullptr ? title : "");
    lv_obj_set_width(title_label, title_width - text_x);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(title_label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, lv_color_hex(colors.text), LV_PART_MAIN);

    const bool has_subtitle = subtitle != nullptr && subtitle[0] != '\0';
    lv_obj_align(title_label, has_subtitle ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID,
                 text_x, has_subtitle ? 20 : 0);
    if (has_subtitle) {
        lv_obj_t* subtitle_label = lv_label_create(row);
        lv_label_set_text(subtitle_label, subtitle);
        lv_obj_set_width(subtitle_label, 350 - text_x);
        lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(subtitle_label, fonts::Small(), LV_PART_MAIN);
        lv_obj_set_style_text_color(subtitle_label, lv_color_hex(colors.muted),
                                    LV_PART_MAIN);
        lv_obj_align(subtitle_label, LV_ALIGN_BOTTOM_LEFT, text_x, -18);
    }
    return row;
}

lv_obj_t* CreateSegment(lv_obj_t* parent, int height) {
    lv_obj_t* segment = lv_obj_create(parent);
    lv_obj_remove_style_all(segment);
    lv_obj_set_size(segment, LV_PCT(100), height);
    lv_obj_set_style_border_color(segment,
                                  lv_color_hex(Theme::Get().colors().border),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(segment, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(segment, metrics::kRadiusControl, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(segment, true, LV_PART_MAIN);
    lv_obj_set_flex_flow(segment, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(segment, LV_OBJ_FLAG_SCROLLABLE);
    return segment;
}

lv_obj_t* AddSegmentButton(lv_obj_t* segment, const char* icon,
                           const char* label, bool selected,
                           lv_event_cb_t callback, void* user_data) {
    const auto& colors = Theme::Get().colors();
    const uint32_t existing_count = lv_obj_get_child_count(segment);
    if (existing_count > 0) {
        lv_obj_t* previous = lv_obj_get_child(
            segment, static_cast<int32_t>(existing_count - 1));
        lv_obj_set_style_border_color(previous, lv_color_hex(colors.border),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_width(previous, 1, LV_PART_MAIN);
        lv_obj_set_style_border_side(previous, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
    }
    lv_obj_t* button = CreateButton(segment);
    lv_obj_remove_style_all(button);
    lv_obj_set_height(button, LV_PCT(100));
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(selected ? colors.accent : colors.background),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(colors.accent_pressed),
                              Selector(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
    if (callback != nullptr) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t* content = lv_obj_create(button);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(content, 12, LV_PART_MAIN);
    lv_obj_center(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
    IgnoreSwipeBack(button, true);

    if (icon != nullptr && icon[0] != '\0') {
        lv_obj_t* icon_label = lv_label_create(content);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, fonts::Icon(), LV_PART_MAIN);
        lv_obj_set_style_text_color(icon_label,
                                    lv_color_hex(selected ? colors.accent_ink
                                                          : colors.text),
                                    LV_PART_MAIN);
    }
    lv_obj_t* text_label = lv_label_create(content);
    lv_label_set_text(text_label, label != nullptr ? label : "");
    lv_obj_set_style_text_font(text_label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(text_label,
                                lv_color_hex(selected ? colors.accent_ink : colors.text),
                                LV_PART_MAIN);
    return button;
}

void SetSegmentButtonSelected(lv_obj_t* button, bool selected) {
    if (button == nullptr) return;
    const auto& colors = Theme::Get().colors();
    lv_obj_set_style_bg_color(
        button, lv_color_hex(selected ? colors.accent : colors.background),
        LV_PART_MAIN);

    lv_obj_t* content = lv_obj_get_child(button, 0);
    if (content == nullptr) return;
    const uint32_t child_count = lv_obj_get_child_count(content);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t* child = lv_obj_get_child(content, static_cast<int32_t>(i));
        if (child != nullptr) {
            lv_obj_set_style_text_color(
                child,
                lv_color_hex(selected ? colors.accent_ink : colors.text),
                LV_PART_MAIN);
        }
    }
}

lv_obj_t* CreateContentPanel(lv_obj_t* parent, int height, int row_gap) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel, row_gap, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

ToolbarParts CreateToolbar(lv_obj_t* parent, const char* title,
                           const char* detail, const char* action,
                           lv_event_cb_t callback, void* user_data) {
    const auto& colors = Theme::Get().colors();
    ToolbarParts parts{};
    parts.root = lv_obj_create(parent);
    lv_obj_remove_style_all(parts.root);
    lv_obj_set_size(parts.root, LV_PCT(100), 58);
    lv_obj_remove_flag(parts.root, LV_OBJ_FLAG_SCROLLABLE);

    parts.title = lv_label_create(parts.root);
    lv_label_set_text(parts.title, title != nullptr ? title : "");
    lv_obj_set_style_text_font(parts.title, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(parts.title, lv_color_hex(colors.text), LV_PART_MAIN);

    const bool has_detail = detail != nullptr && detail[0] != '\0';
    lv_obj_align(parts.title, has_detail ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID,
                 0, 0);
    if (has_detail) {
        parts.detail = lv_label_create(parts.root);
        lv_label_set_text(parts.detail, detail);
        lv_obj_set_style_text_font(parts.detail, fonts::Small(), LV_PART_MAIN);
        lv_obj_set_style_text_color(parts.detail, lv_color_hex(colors.muted),
                                    LV_PART_MAIN);
        lv_obj_align(parts.detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    if (action != nullptr && callback != nullptr) {
        parts.action = CreateButton(parts.root);
        lv_obj_set_size(parts.action, 112, 48);
        lv_obj_align(parts.action, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(parts.action, 7, LV_PART_MAIN);
        lv_obj_set_style_bg_color(parts.action, lv_color_hex(colors.accent), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(parts.action, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(parts.action, callback, LV_EVENT_CLICKED, user_data);
        IgnoreSwipeBack(parts.action, true);

        parts.action_label = lv_label_create(parts.action);
        lv_label_set_text(parts.action_label, action);
        lv_obj_set_style_text_font(parts.action_label, fonts::SmallBold(), LV_PART_MAIN);
        lv_obj_set_style_text_color(parts.action_label,
                                    lv_color_hex(colors.accent_ink), LV_PART_MAIN);
        lv_obj_center(parts.action_label);
    }
    return parts;
}

lv_obj_t* CreateDividerList(lv_obj_t* parent, int height, bool outlined,
                            int radius) {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* list = CreateContentPanel(parent, height, 0);
    lv_obj_set_style_border_color(list, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, outlined ? 1 : 0, LV_PART_MAIN);
    lv_obj_set_style_radius(list, radius, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(list, radius > 0, LV_PART_MAIN);
    return list;
}

CompactRowParts CreateCompactRow(
    lv_obj_t* parent, const char* icon, const char* title, const char* detail,
    const char* trailing, int height, bool highlighted, bool divider,
    lv_event_cb_t callback, void* user_data) {
    const auto& colors = Theme::Get().colors();
    CompactRowParts parts{};
    parts.root = callback != nullptr ? CreateButton(parent) : lv_obj_create(parent);
    lv_obj_remove_style_all(parts.root);
    lv_obj_set_size(parts.root, LV_PCT(100), height);
    lv_obj_set_style_bg_color(parts.root,
                              lv_color_hex(highlighted ? colors.accent
                                                       : colors.background),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parts.root,
                            highlighted ? kAccentSoftOpacity : LV_OPA_TRANSP,
                            LV_PART_MAIN);
    lv_obj_set_style_bg_color(parts.root, lv_color_hex(colors.accent_pressed),
                              Selector(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(parts.root, LV_OPA_COVER,
                            Selector(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_color(parts.root, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(parts.root, divider ? 1 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(parts.root, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(parts.root, 12, LV_PART_MAIN);
    lv_obj_remove_flag(parts.root, LV_OBJ_FLAG_SCROLLABLE);
    if (callback != nullptr) {
        lv_obj_add_event_cb(parts.root, callback, LV_EVENT_CLICKED, user_data);
    }

    int text_x = 0;
    if (icon != nullptr && icon[0] != '\0') {
        lv_obj_t* icon_label = lv_label_create(parts.root);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, fonts::Icon(), LV_PART_MAIN);
        lv_obj_set_style_text_color(icon_label, lv_color_hex(colors.accent), LV_PART_MAIN);
        lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 0, 0);
        text_x = 42;
    }

    parts.title = lv_label_create(parts.root);
    lv_label_set_text(parts.title, title != nullptr ? title : "");
    lv_obj_set_width(parts.title, 370);
    lv_label_set_long_mode(parts.title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(parts.title, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(parts.title, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_align(parts.title, detail != nullptr ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID,
                 text_x, detail != nullptr ? 7 : 0);

    if (detail != nullptr) {
        parts.detail = lv_label_create(parts.root);
        lv_label_set_text(parts.detail, detail);
        lv_obj_set_width(parts.detail, 400);
        lv_label_set_long_mode(parts.detail, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(parts.detail, fonts::Small(), LV_PART_MAIN);
        lv_obj_set_style_text_color(parts.detail, lv_color_hex(colors.muted), LV_PART_MAIN);
        lv_obj_align(parts.detail, LV_ALIGN_BOTTOM_LEFT, text_x, -7);
    }

    if (trailing != nullptr) {
        parts.trailing = lv_label_create(parts.root);
        lv_label_set_text(parts.trailing, trailing);
        lv_obj_set_style_text_font(parts.trailing, fonts::MediumBold(), LV_PART_MAIN);
        lv_obj_set_style_text_color(parts.trailing, lv_color_hex(colors.accent),
                                    LV_PART_MAIN);
        lv_obj_align(parts.trailing, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return parts;
}

lv_obj_t* CreateChoicePanel(lv_obj_t* parent, const char* icon,
                            const char* title, const char* detail,
                            const char* action, lv_event_cb_t callback,
                            void* user_data, bool selected) {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), 270);
    lv_obj_set_style_border_color(card, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon_label = lv_label_create(card);
    lv_label_set_text(icon_label, icon != nullptr ? icon : "");
    lv_obj_set_style_text_font(icon_label, fonts::Icon(), LV_PART_MAIN);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_align(icon_label, LV_ALIGN_CENTER, 0, -86);

    lv_obj_t* heading = lv_label_create(card);
    lv_label_set_text(heading, title != nullptr ? title : "");
    lv_obj_set_style_text_font(heading, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(heading, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_align(heading, LV_ALIGN_CENTER, 0, -38);

    if (detail != nullptr && detail[0] != '\0') {
        lv_obj_t* copy = lv_label_create(card);
        lv_label_set_text(copy, detail);
        lv_obj_set_width(copy, LV_PCT(86));
        lv_label_set_long_mode(copy, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(copy, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(copy, fonts::Small(), LV_PART_MAIN);
        lv_obj_set_style_text_color(copy, lv_color_hex(colors.muted), LV_PART_MAIN);
        lv_obj_align(copy, LV_ALIGN_CENTER, 0, 8);
    }

    lv_obj_t* button = CreateButton(card);
    lv_obj_set_size(button, LV_PCT(86), 58);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    if (callback != nullptr) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }
    IgnoreSwipeBack(button, true);

    lv_obj_t* action_label = lv_label_create(button);
    lv_label_set_text(action_label, selected ? "当前使用" : (action != nullptr ? action : ""));
    lv_obj_set_style_text_font(action_label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(action_label, lv_color_hex(colors.accent_ink), LV_PART_MAIN);
    lv_obj_center(action_label);
    return card;
}

lv_obj_t* AddSwitch(lv_obj_t* row, bool checked, lv_event_cb_t callback,
                    void* user_data) {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* control = lv_switch_create(row);
    AttachButtonHaptic(control);
    lv_obj_set_size(control, 88, 48);
    lv_obj_align(control, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(colors.raised), LV_PART_MAIN);
    lv_obj_set_style_border_color(control, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(control, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        control, lv_color_hex(colors.accent),
        Selector(LV_PART_MAIN, LV_STATE_CHECKED));
    lv_obj_set_style_radius(control, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(control, lv_color_hex(colors.accent),
                              Selector(LV_PART_INDICATOR, LV_STATE_CHECKED));
    lv_obj_set_style_radius(control, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(control, lv_color_hex(colors.muted), LV_PART_KNOB);
    lv_obj_set_style_bg_color(control, lv_color_hex(colors.accent_ink),
                              Selector(LV_PART_KNOB, LV_STATE_CHECKED));
    lv_obj_set_style_radius(control, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    if (checked) lv_obj_add_state(control, LV_STATE_CHECKED);
    if (callback != nullptr) {
        lv_obj_add_event_cb(control, callback, LV_EVENT_VALUE_CHANGED, user_data);
    }

    // The whole settings card is the touch target. Child controls can bubble
    // CLICKED events to the row, so ToggleSwitchFromRow ignores non-row targets
    // to ensure a direct switch tap still toggles exactly once.
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(
        row, kAccentSoftOpacity,
        Selector(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_event_cb(row, ToggleSwitchFromRow, LV_EVENT_CLICKED, control);
    IgnoreSwipeBack(control, true);
    return control;
}

lv_obj_t* AddChevron(lv_obj_t* row) {
    lv_obj_t* arrow = lv_label_create(row);
    lv_label_set_text(arrow, FONT_AWESOME_ANGLE_RIGHT);
    lv_obj_set_style_text_font(arrow, fonts::Icon(), LV_PART_MAIN);
    lv_obj_set_style_text_color(arrow, lv_color_hex(Theme::Get().colors().muted),
                                LV_PART_MAIN);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    return arrow;
}

lv_obj_t* AddValueLabel(lv_obj_t* row, const char* text, int width) {
    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(Theme::Get().colors().accent),
                                LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, 0, 0);
    return label;
}

lv_obj_t* AddSlider(lv_obj_t* row, int min_value, int max_value, int value,
                    lv_event_cb_t callback, int right_offset) {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* slider = lv_slider_create(row);
    lv_obj_set_size(slider, kSettingsRangeSliderWidth, 24);
    lv_obj_align(slider, LV_ALIGN_RIGHT_MID, -right_offset, 0);
    lv_slider_set_range(slider, min_value, max_value);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(colors.accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(colors.text), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);
    auto* guard = new SliderGuard{value};
    lv_obj_add_event_cb(slider, GuardSliderRelease, LV_EVENT_PRESSED, guard);
    lv_obj_add_event_cb(slider, GuardSliderRelease, LV_EVENT_VALUE_CHANGED, guard);
    lv_obj_add_event_cb(slider, GuardSliderRelease, LV_EVENT_RELEASED, guard);
    lv_obj_add_event_cb(slider, GuardSliderRelease, LV_EVENT_PRESS_LOST, guard);
    lv_obj_add_event_cb(slider, GuardSliderRelease, LV_EVENT_DELETE, guard);
    if (callback != nullptr) {
        lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    IgnoreSwipeBack(slider, true);
    return slider;
}

}  // namespace agent_ui::ui_components
