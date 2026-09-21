#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "lvgl.h"

namespace agent_ui::ui_components {

// Geometry shared by the three General settings range rows. Keep the value
// column wide enough for the longest standby label ("30 分钟") while leaving
// a small gap before the slider and enough room for the title/icon column.
constexpr int kSettingsRangeTitleWidth = 186;
constexpr int kSettingsRangeSliderWidth = 300;
constexpr int kSettingsRangeSliderRightOffset = 120;
constexpr int kSettingsRangeValueWidth = 112;

struct ToolbarParts {
    lv_obj_t* root = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* detail = nullptr;
    lv_obj_t* action = nullptr;
    lv_obj_t* action_label = nullptr;
};

struct CompactRowParts {
    lv_obj_t* root = nullptr;
    lv_obj_t* icon = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* detail = nullptr;
    lv_obj_t* trailing = nullptr;
};

struct ActionButtonParts {
    lv_obj_t* root = nullptr;
    lv_obj_t* icon = nullptr;
    lv_obj_t* label = nullptr;
};

struct DrawerParts {
    lv_obj_t *overlay = nullptr, *surface = nullptr, *content = nullptr, *tabs = nullptr;
};
struct StatusCardParts {
    lv_obj_t *root = nullptr, *heading = nullptr, *title = nullptr;
    lv_obj_t *status = nullptr, *dot = nullptr, *check = nullptr;
};
struct ChoiceSliderParts {
    lv_obj_t *root = nullptr, *slider = nullptr, *value = nullptr;
};

enum class LineIcon { Tasks, Sliders, Plus, Sparkles, Zap, Monitor, Keyboard, Link, Eye };
lv_obj_t* CreateLineIcon(lv_obj_t* parent, LineIcon icon, int size = 28);
void SetCompactRowIcon(CompactRowParts& row, LineIcon icon, int size = 28);

// A drawer has a scroll viewport above its own tab bar, independent of the
// application's full-width bottom action bar.
DrawerParts CreateRightDrawer(lv_obj_t* parent, lv_event_cb_t dismiss);
ActionButtonParts AddDrawerTab(lv_obj_t* bar, LineIcon icon, const char* label,
                               lv_event_cb_t callback, void* data = nullptr);
void SetDrawerTabSelected(lv_obj_t* button, bool selected);
void SetLabelTextIfChanged(lv_obj_t* label, const char* text);
void StyleSettingsCard(lv_obj_t* card, bool selected = false, bool outlined = true);
StatusCardParts CreateStatusCard(lv_obj_t* parent, const char* heading,
                                 lv_event_cb_t callback, void* data = nullptr);
lv_obj_t* CreateDropdownField(lv_obj_t* parent, LineIcon icon,
                              lv_event_cb_t callback);
// Preview while pressed; commit one final choice on release. Context identifies
// the setting being edited, so changing targets cancels the active gesture.
ChoiceSliderParts CreateChoiceSlider(lv_obj_t* parent, const char* title, lv_event_cb_t commit);
void UpdateChoiceSlider(const ChoiceSliderParts& parts, const std::vector<std::string>& options,
                        uint32_t selected, bool enabled, bool pending = false,
                        const std::string& context = {});
lv_obj_t* CreateTextField(lv_obj_t* parent, const char* placeholder,
                          lv_event_cb_t callback, bool password = false);

constexpr size_t kVoiceWaveCount = 6;

struct VoiceButtonParts {
    lv_obj_t* root = nullptr;
    lv_obj_t* icon = nullptr;
    lv_obj_t* label = nullptr;
    lv_obj_t* record_dot = nullptr;
    lv_obj_t* waves[kVoiceWaveCount]{};
};

// All reusable buttons created here include the shared medium click haptic.
lv_obj_t* CreateButton(lv_obj_t* parent);
// Shared modal primitives keep blocking overlays consistent across apps.
lv_obj_t* CreateModalOverlay(lv_obj_t* parent);
lv_obj_t* CreateModalSurface(lv_obj_t* overlay, int width, int height);
lv_obj_t* CreateBottomActionBar(lv_obj_t* parent, int y = -1);
ActionButtonParts AddBottomActionButton(
    lv_obj_t* bar, const char* icon, const char* label,
    lv_event_cb_t callback, void* user_data = nullptr, bool danger = false);
ActionButtonParts AddBottomPrimaryButton(
    lv_obj_t* bar, const char* icon, const char* label,
    lv_event_cb_t callback, void* user_data = nullptr);
VoiceButtonParts AddBottomVoiceButton(
    lv_obj_t* bar, const char* label, lv_event_cb_t callback,
    void* user_data = nullptr);
void SetVoiceButtonAnimating(const VoiceButtonParts& parts, bool active);
lv_obj_t* AddBottomActionSpacer(lv_obj_t* bar);
ActionButtonParts AddWideActionButton(
    lv_obj_t* parent, const char* icon, const char* label,
    lv_event_cb_t callback, void* user_data = nullptr);

// Shared building blocks for settings-like screens. They intentionally own
// only presentation; feature screens remain responsible for state and events.
void StylePanel(lv_obj_t* panel);
lv_obj_t* CreateSectionHeading(lv_obj_t* parent, const char* title,
                               const char* subtitle = nullptr);
lv_obj_t* CreateRow(lv_obj_t* parent, const char* icon, const char* title,
                    const char* subtitle, int height = 104,
                    int title_width = 310);
lv_obj_t* CreateSegment(lv_obj_t* parent, int height = 72);
lv_obj_t* AddSegmentButton(lv_obj_t* segment, const char* icon,
                           const char* label, bool selected,
                           lv_event_cb_t callback, void* user_data = nullptr);
void SetSegmentButtonSelected(lv_obj_t* button, bool selected);
lv_obj_t* CreateContentPanel(lv_obj_t* parent, int height = LV_SIZE_CONTENT,
                             int row_gap = 0);
ToolbarParts CreateToolbar(lv_obj_t* parent, const char* title,
                           const char* detail, const char* action = nullptr,
                           lv_event_cb_t callback = nullptr,
                           void* user_data = nullptr);
lv_obj_t* CreateDividerList(lv_obj_t* parent, int height,
                            bool outlined = false, int radius = 0);
CompactRowParts CreateCompactRow(
    lv_obj_t* parent, const char* icon, const char* title, const char* detail,
    const char* trailing, int height = 72, bool highlighted = false,
    bool divider = true, lv_event_cb_t callback = nullptr,
    void* user_data = nullptr);
lv_obj_t* CreateChoicePanel(lv_obj_t* parent, const char* icon,
                            const char* title, const char* detail,
                            const char* action, lv_event_cb_t callback,
                            void* user_data = nullptr, bool selected = false);
lv_obj_t* AddSwitch(lv_obj_t* row, bool checked, lv_event_cb_t callback,
                    void* user_data = nullptr);
lv_obj_t* AddChevron(lv_obj_t* row);
lv_obj_t* AddValueLabel(lv_obj_t* row, const char* text, int width = 64);
lv_obj_t* AddSlider(lv_obj_t* row, int min_value, int max_value, int value,
                    lv_event_cb_t callback,
                    int right_offset = kSettingsRangeSliderRightOffset);

}  // namespace agent_ui::ui_components
