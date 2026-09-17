#include "settings_panels_ui.h"

#include <array>
#include <font_awesome.h>

#include "components/haptic_feedback.h"
#include "components/ui_components.h"
#include "components/system_keyboard.h"
#include "core/fonts.h"
#include "core/theme.h"
#include "xiaozhi_server_settings.h"

namespace agent_ui::settings_panels_ui {
namespace {

namespace controls = ui_components;

lv_obj_t* CreateAccentGrid(lv_obj_t* parent, size_t selected,
                           lv_event_cb_t callback) {
    const auto& theme = Theme::Get();
    lv_obj_t* grid = controls::CreateContentPanel(parent, 58);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(grid, 12, LV_PART_MAIN);

    struct AccentSwatch {
        AccentPreset preset;
        uint32_t color;
    };
    constexpr std::array<AccentSwatch, 4> swatches = {
        AccentSwatch{AccentPreset::Coral, 0xFF6D00},
        AccentSwatch{AccentPreset::Cobalt, 0x0B44D8},
        AccentSwatch{AccentPreset::Teal, 0x008B83},
        AccentSwatch{AccentPreset::Amber, 0xC77B00},
    };
    for (const auto& item : swatches) {
        const bool is_selected =
            selected == static_cast<size_t>(item.preset);
        lv_obj_t* swatch = controls::CreateButton(grid);
        lv_obj_remove_style_all(swatch);
        lv_obj_set_height(swatch, 58);
        lv_obj_set_flex_grow(swatch, 1);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(theme.colors().surface),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(swatch, is_selected ? 4 : 1,
                                      LV_PART_MAIN);
        lv_obj_set_style_border_color(
            swatch,
            lv_color_hex(is_selected ? theme.colors().text
                                     : theme.colors().border),
            LV_PART_MAIN);
        lv_obj_set_style_radius(swatch, 12, LV_PART_MAIN);
        if (callback != nullptr) {
            lv_obj_add_event_cb(swatch, callback, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(
                                    static_cast<uintptr_t>(item.preset)));
        }

        lv_obj_t* color = lv_obj_create(swatch);
        lv_obj_remove_style_all(color);
        lv_obj_set_size(color, LV_PCT(72), 32);
        lv_obj_set_style_bg_color(color, lv_color_hex(item.color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(color, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(color, 4, LV_PART_MAIN);
        lv_obj_center(color);
        lv_obj_remove_flag(color, LV_OBJ_FLAG_CLICKABLE);
    }
    return grid;
}

lv_obj_t* CreateRangeRow(lv_obj_t* parent, const char* icon, const char* title,
                         const char* subtitle, int min_value, int max_value,
                         int value, lv_event_cb_t callback,
                         lv_obj_t** value_label) {
    lv_obj_t* row = controls::CreateRow(
        parent, icon, title, subtitle, 104, controls::kSettingsRangeTitleWidth);
    controls::AddSlider(row, min_value, max_value, value, callback);
    *value_label = controls::AddValueLabel(
        row, "", controls::kSettingsRangeValueWidth);
    return row;
}

void AddAboutItem(lv_obj_t* list, const char* label, const char* value,
                  bool divider) {
    auto row = controls::CreateCompactRow(list, nullptr, label, nullptr, value,
                                          88, false, divider);
    lv_obj_set_style_text_color(row.title,
                                lv_color_hex(Theme::Get().colors().muted),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(row.trailing,
                                lv_color_hex(Theme::Get().colors().text),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(row.trailing, fonts::MediumBold(), LV_PART_MAIN);
}

}  // namespace

GeneralHandles BuildGeneral(lv_obj_t* parent, const GeneralModel& model,
                            const GeneralCallbacks& callbacks) {
    GeneralHandles handles{};
    controls::CreateSectionHeading(parent, "外观模式");
    lv_obj_t* appearance = controls::CreateSegment(parent);
    controls::AddSegmentButton(
        appearance, FONT_AWESOME_SUN, "浅色", model.appearance == 0,
        callbacks.appearance, reinterpret_cast<void*>(static_cast<uintptr_t>(0)));
    controls::AddSegmentButton(
        appearance, FONT_AWESOME_MOON, "深色", model.appearance == 1,
        callbacks.appearance, reinterpret_cast<void*>(static_cast<uintptr_t>(1)));

    controls::CreateSectionHeading(parent, "强调色");
    CreateAccentGrid(parent, model.accent, callbacks.accent);
    CreateRangeRow(parent, FONT_AWESOME_SUN, "屏幕亮度", nullptr,
                   model.brightness_min, 100, model.brightness,
                   callbacks.brightness,
                   &handles.brightness_value);
    CreateRangeRow(parent, FONT_AWESOME_VOLUME_HIGH, "系统音量", nullptr,
                   0, 100, model.volume, callbacks.volume,
                   &handles.volume_value);
    CreateRangeRow(parent, FONT_AWESOME_MOON, "待机时长", nullptr,
                   0, 4, model.standby_index, callbacks.standby,
                   &handles.standby_value);
    return handles;
}

AiHandles BuildAi(lv_obj_t* parent, const AiModel& model,
                  const AiCallbacks& callbacks) {
    AiHandles handles;
    controls::CreateSectionHeading(parent, "小智");
    auto* segment = controls::CreateSegment(parent);
    handles.official_tab = controls::AddSegmentButton(
        segment, FONT_AWESOME_CLOUD, "官方小智", !model.custom_server,
        callbacks.server_changed, nullptr);
    handles.custom_tab = controls::AddSegmentButton(
        segment, FONT_AWESOME_LINK, "自建服务器", model.custom_server,
        callbacks.server_changed, reinterpret_cast<void*>(static_cast<uintptr_t>(1)));

    handles.custom_panel = controls::CreateContentPanel(parent, LV_SIZE_CONTENT, 12);
    auto* field = controls::CreateContentPanel(handles.custom_panel, 144, 12);
    auto* label = lv_label_create(field);
    lv_label_set_text(label, "服务器配置地址");
    lv_obj_set_style_text_font(label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(Theme::Get().colors().text), LV_PART_MAIN);
    handles.url = lv_textarea_create(field);
    StyleTextInput(handles.url);
    lv_textarea_set_one_line(handles.url, true);
    lv_textarea_set_max_length(handles.url, xiaozhi_server_settings::kMaxUrlLength);
    lv_obj_set_size(handles.url, LV_PCT(100), 96);
    lv_obj_set_style_pad_ver(handles.url, 22, LV_PART_MAIN);
    lv_obj_set_style_text_font(handles.url, fonts::Medium(), LV_PART_MAIN);
    lv_textarea_set_text(handles.url, model.custom_url);
    if (callbacks.url_ready) {
        lv_obj_add_event_cb(handles.url, callbacks.url_ready, LV_EVENT_READY, nullptr);
    }
    Keyboard::Get().Bind(handles.url, "服务器配置地址", LV_KEYBOARD_MODE_TEXT_LOWER);
    if (!model.custom_server) lv_obj_add_flag(handles.custom_panel, LV_OBJ_FLAG_HIDDEN);

    handles.status = lv_label_create(parent);
    lv_label_set_text(handles.status, model.status);
    lv_obj_set_width(handles.status, LV_PCT(100));
    lv_label_set_long_mode(handles.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(handles.status, fonts::Small(), LV_PART_MAIN);
    lv_obj_set_style_text_color(handles.status, lv_color_hex(Theme::Get().colors().muted), LV_PART_MAIN);
    if (!model.status || !model.status[0]) lv_obj_add_flag(handles.status, LV_OBJ_FLAG_HIDDEN);
    handles.apply = controls::AddWideActionButton(
        parent, FONT_AWESOME_ARROWS_ROTATE, "保存并重启连接", callbacks.apply).root;
    lv_obj_t* wake_row = controls::CreateRow(
        parent, FONT_AWESOME_MICROPHONE, "语音唤醒", nullptr);
    controls::AddSwitch(wake_row, model.wake_enabled, callbacks.wake_changed);
    return handles;
}

void BuildLanguage(lv_obj_t* parent, const LanguageOption* options,
                   size_t option_count, lv_event_cb_t callback) {
    controls::CreateSectionHeading(parent, "语言");
    const auto& colors = Theme::Get().colors();
    lv_obj_t* list = controls::CreateDividerList(
        parent, static_cast<int>(option_count) * 88, true, 14);
    for (size_t i = 0; i < option_count; ++i) {
        const auto& option = options[i];
        auto row = controls::CreateCompactRow(
            list, nullptr, option.label, nullptr, nullptr, 88, false,
            i + 1 < option_count, callback,
            reinterpret_cast<void*>(option.id));
        if (!option.selected) continue;
        lv_obj_set_style_text_color(row.title, lv_color_hex(colors.accent),
                                    LV_PART_MAIN);
        lv_obj_t* check = lv_label_create(row.root);
        lv_label_set_text(check, FONT_AWESOME_CHECK);
        lv_obj_set_style_text_font(check, fonts::Icon(), LV_PART_MAIN);
        lv_obj_set_style_text_color(check, lv_color_hex(colors.accent),
                                    LV_PART_MAIN);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

void BuildAbout(lv_obj_t* parent, const AboutInfo& info) {
    controls::CreateSectionHeading(parent, "关于");
    lv_obj_t* list = controls::CreateDividerList(parent, 352);
    AddAboutItem(list, "产品", info.product, true);
    AddAboutItem(list, "版本", info.version, true);
    AddAboutItem(list, "设备", info.device, true);
    AddAboutItem(list, "显示", info.display, false);
}

}  // namespace agent_ui::settings_panels_ui
