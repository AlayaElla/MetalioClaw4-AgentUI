#pragma once

#include <cstdint>

#include "agent_ui_types.h"
#include "lvgl.h"

namespace agent_ui {

struct ThemeColors {
    uint32_t background;
    uint32_t surface;
    uint32_t raised;
    uint32_t border;
    uint32_t text;
    uint32_t muted;
    uint32_t accent;
    uint32_t accent_pressed;
    uint32_t accent_ink;
    uint32_t danger;
    uint32_t warning;
};

class Theme {
public:
    static Theme& Get();

    void Initialize();
    AccentPreset accent_preset() const { return accent_preset_; }
    void SetAccentPreset(AccentPreset preset);
    AppearanceMode appearance_mode() const { return appearance_mode_; }
    void SetAppearanceMode(AppearanceMode mode);

    const ThemeColors& colors() const { return colors_; }
    lv_color_t Color(uint32_t value) const { return lv_color_hex(value); }

private:
    Theme() = default;
    void ApplyPreset(AccentPreset preset);

    AccentPreset accent_preset_ = AccentPreset::Coral;
    AppearanceMode appearance_mode_ = AppearanceMode::Dark;
    ThemeColors colors_{};
};

namespace metrics {
constexpr int kDisplaySize = 720;
constexpr int kStatusBarHeight = 62;
constexpr int kAppHeaderHeight = 98;
constexpr int kContentTop = kStatusBarHeight + kAppHeaderHeight;
constexpr int kBottomActionBarHeight = 132;
constexpr int kBottomActionBarY = kDisplaySize - kBottomActionBarHeight;
constexpr int kBottomActionContentHeight =
    kBottomActionBarY - kStatusBarHeight;
constexpr int kRadiusSmall = 8;
constexpr int kRadiusControl = 14;
constexpr int kRadiusPanel = 15;
constexpr int kRadius = kRadiusSmall;
constexpr int kPagePadding = 34;
constexpr int kHeaderPadding = 30;
constexpr int kSystemPadding = 42;
constexpr int kTouchTarget = 56;
constexpr int kBottomActionHeight = 80;
constexpr int kBottomPrimaryActionHeight = 104;
constexpr int kTransitionMs = 180;
}  // namespace metrics

void StyleRoot(lv_obj_t* root);
void StyleSurface(lv_obj_t* object, bool raised = false);
void StyleButton(lv_obj_t* button, bool accent = false);
void StyleTextInput(lv_obj_t* textarea);
void StyleLabel(lv_obj_t* label, bool muted = false);

}  // namespace agent_ui
