#include "theme.h"

#include <algorithm>

#include "fonts.h"
#include "settings.h"

namespace agent_ui {
namespace {

constexpr ThemeColors kBaseColors = {
    .background = 0xF7F6F3,
    .surface = 0xFFFFFF,
    .raised = 0xEEEDE9,
    .border = 0xB7B7B3,
    .text = 0x0D0E0D,
    .muted = 0x6D706E,
    .accent = 0x0B44D8,
    .accent_pressed = 0x0737B4,
    .accent_ink = 0xFFFFFF,
    .danger = 0xE12D25,
    .warning = 0xC77B00,
};

constexpr ThemeColors kDarkBaseColors = {
    .background = 0x101211,
    .surface = 0x191C1A,
    .raised = 0x222624,
    .border = 0x434844,
    .text = 0xF3F2ED,
    .muted = 0xA2A59F,
    .accent = 0x6F8CFF,
    .accent_pressed = 0x98AAFF,
    .accent_ink = 0x101211,
    .danger = 0xFF5B51,
    .warning = 0xF0B449,
};

}  // namespace

Theme& Theme::Get() {
    static Theme instance;
    return instance;
}

void Theme::Initialize() {
    Settings settings("agent_ui", true);
    const int appearance = settings.GetInt(
        "appearance", static_cast<int>(AppearanceMode::Light));
    appearance_mode_ = static_cast<AppearanceMode>(std::clamp(
        appearance, 0, static_cast<int>(AppearanceMode::Dark)));
    const int value = settings.GetInt("accent", static_cast<int>(AccentPreset::Cobalt));
    const int maximum = static_cast<int>(AccentPreset::Amber);
    ApplyPreset(static_cast<AccentPreset>(std::clamp(value, 0, maximum)));
}

void Theme::SetAccentPreset(AccentPreset preset) {
    ApplyPreset(preset);
    Settings settings("agent_ui", true);
    settings.SetInt("accent", static_cast<int>(preset));
}

void Theme::SetAppearanceMode(AppearanceMode mode) {
    appearance_mode_ = mode;
    ApplyPreset(accent_preset_);
    Settings settings("agent_ui", true);
    settings.SetInt("appearance", static_cast<int>(mode));
}

void Theme::ApplyPreset(AccentPreset preset) {
    colors_ = appearance_mode_ == AppearanceMode::Dark ? kDarkBaseColors
                                                       : kBaseColors;
    accent_preset_ = preset;
    const bool dark = appearance_mode_ == AppearanceMode::Dark;
    switch (preset) {
        case AccentPreset::Teal:
            colors_.accent = dark ? 0x37C7BA : 0x008B83;
            colors_.accent_pressed = dark ? 0x65D7CC : 0x006E68;
            break;
        case AccentPreset::Coral:
            colors_.accent = dark ? 0xFF786E : 0xE74638;
            colors_.accent_pressed = dark ? 0xFF9A93 : 0xC33227;
            break;
        case AccentPreset::Amber:
            colors_.accent = dark ? 0xF0B449 : 0xC77B00;
            colors_.accent_pressed = dark ? 0xF5C56F : 0xA76500;
            colors_.accent_ink = 0x11130F;
            break;
        case AccentPreset::Cobalt:
        default:
            colors_.accent = dark ? 0x6F8CFF : 0x0B44D8;
            colors_.accent_pressed = dark ? 0x98AAFF : 0x0737B4;
            break;
    }
}

void StyleRoot(lv_obj_t* root) {
    if (root == nullptr) return;
    const auto& colors = Theme::Get().colors();
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, metrics::kDisplaySize, metrics::kDisplaySize);
    lv_obj_set_style_bg_color(root, lv_color_hex(colors.background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
}

void StyleSurface(lv_obj_t* object, bool raised) {
    if (object == nullptr) return;
    const auto& colors = Theme::Get().colors();
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_color(
        object, lv_color_hex(raised ? colors.raised : colors.surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(object, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(object, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(object, metrics::kRadiusPanel, LV_PART_MAIN);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

void StyleButton(lv_obj_t* button, bool accent) {
    if (button == nullptr) return;
    const auto& colors = Theme::Get().colors();
    lv_obj_remove_style_all(button);
    lv_obj_set_style_bg_color(
        button, lv_color_hex(accent ? colors.accent : colors.raised), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(colors.accent_pressed),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(
        button, lv_color_hex(accent ? colors.accent : colors.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(button, metrics::kRadiusControl, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
}

void StyleLabel(lv_obj_t* label, bool muted) {
    if (label == nullptr) return;
    const auto& colors = Theme::Get().colors();
    lv_obj_set_style_text_font(label, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(
        label, lv_color_hex(muted ? colors.muted : colors.text), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
}

}  // namespace agent_ui
