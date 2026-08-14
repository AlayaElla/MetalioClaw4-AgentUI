#include "settings_panels_ui.h"

#include <array>
#include <font_awesome.h>

#include "components/haptic_feedback.h"
#include "components/system_keyboard.h"
#include "components/ui_components.h"
#include "core/fonts.h"
#include "core/theme.h"

namespace agent_ui::settings_panels_ui {
namespace {

namespace controls = ui_components;

constexpr int kHermesFieldHeight = 144;
constexpr int kHermesInputHeight = 96;
constexpr int kHermesInputVerticalPadding = 22;

lv_obj_t* CreateHermesInput(lv_obj_t* parent, const char* title) {
    lv_obj_t* field = controls::CreateContentPanel(parent, kHermesFieldHeight, 10);
    lv_obj_t* label = lv_label_create(field);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(Theme::Get().colors().text),
                                LV_PART_MAIN);

    lv_obj_t* textarea = lv_textarea_create(field);
    lv_obj_set_size(textarea, LV_PCT(100), kHermesInputHeight);
    lv_obj_set_style_pad_ver(textarea, kHermesInputVerticalPadding, LV_PART_MAIN);
    lv_obj_set_style_text_font(textarea, fonts::Medium(), LV_PART_MAIN);
    return textarea;
}

lv_obj_t* CreateAccentGrid(lv_obj_t* parent, size_t selected,
                           lv_event_cb_t callback) {
    const auto& theme = Theme::Get();
    lv_obj_t* grid = controls::CreateContentPanel(parent, 58);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(grid, 12, LV_PART_MAIN);

    constexpr std::array<uint32_t, 4> colors = {
        0x0B44D8, 0x008B83, 0xE74638, 0xC77B00,
    };
    for (size_t i = 0; i < colors.size(); ++i) {
        lv_obj_t* swatch = controls::CreateButton(grid);
        lv_obj_remove_style_all(swatch);
        lv_obj_set_height(swatch, 58);
        lv_obj_set_flex_grow(swatch, 1);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(theme.colors().surface),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(swatch, selected == i ? 4 : 1,
                                      LV_PART_MAIN);
        lv_obj_set_style_border_color(
            swatch,
            lv_color_hex(selected == i ? theme.colors().text
                                       : theme.colors().border),
            LV_PART_MAIN);
        lv_obj_set_style_radius(swatch, 12, LV_PART_MAIN);
        if (callback != nullptr) {
            lv_obj_add_event_cb(swatch, callback, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(i));
        }

        lv_obj_t* color = lv_obj_create(swatch);
        lv_obj_remove_style_all(color);
        lv_obj_set_size(color, LV_PCT(72), 32);
        lv_obj_set_style_bg_color(color, lv_color_hex(colors[i]), LV_PART_MAIN);
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
    AiHandles handles{};
    controls::CreateSectionHeading(parent, "AI");
    lv_obj_t* provider = controls::CreateSegment(parent);
    controls::AddSegmentButton(
        provider, FONT_AWESOME_MICROPHONE, "小智", !model.hermes_selected,
        callbacks.provider_changed, reinterpret_cast<void*>(static_cast<uintptr_t>(0)));
    controls::AddSegmentButton(
        provider, FONT_AWESOME_LINK, "Hermes", model.hermes_selected,
        callbacks.provider_changed, reinterpret_cast<void*>(static_cast<uintptr_t>(1)));
    lv_obj_t* wake_row = controls::CreateRow(
        parent, FONT_AWESOME_MICROPHONE, "语音唤醒", nullptr);
    controls::AddSwitch(wake_row, model.wake_enabled, callbacks.wake_changed);

    if (model.hermes_selected) {
        controls::CreateSectionHeading(parent, "Hermes Agent");

        handles.hermes_dashboard_url = CreateHermesInput(parent, "Hermes 服务地址");
        lv_textarea_set_one_line(handles.hermes_dashboard_url, true);
        lv_textarea_set_max_length(handles.hermes_dashboard_url, 192);
        lv_textarea_set_placeholder_text(handles.hermes_dashboard_url,
            "默认 http://192.168.50.149:9119");
        lv_textarea_set_text(handles.hermes_dashboard_url,
            model.hermes_dashboard_url != nullptr ? model.hermes_dashboard_url : "");
        Keyboard::Get().Bind(handles.hermes_dashboard_url, "Hermes 服务地址");
        if (callbacks.hermes_field_committed != nullptr) {
            lv_obj_add_event_cb(handles.hermes_dashboard_url,
                                callbacks.hermes_field_committed, LV_EVENT_READY, nullptr);
        }

        handles.hermes_username = CreateHermesInput(parent, "Dashboard 用户名");
        lv_textarea_set_one_line(handles.hermes_username, true);
        lv_textarea_set_max_length(handles.hermes_username, 128);
        lv_textarea_set_placeholder_text(handles.hermes_username, "Dashboard 用户名");
        lv_textarea_set_text(handles.hermes_username,
            model.hermes_username != nullptr ? model.hermes_username : "");
        Keyboard::Get().Bind(handles.hermes_username, "Dashboard 用户名");
        if (callbacks.hermes_field_committed != nullptr) {
            lv_obj_add_event_cb(handles.hermes_username,
                                callbacks.hermes_field_committed, LV_EVENT_READY, nullptr);
        }

        handles.hermes_password = CreateHermesInput(parent, "Dashboard 密码");
        lv_textarea_set_one_line(handles.hermes_password, true);
        lv_textarea_set_max_length(handles.hermes_password, 512);
        lv_textarea_set_password_mode(handles.hermes_password, true);
        lv_textarea_set_placeholder_text(handles.hermes_password,
            model.hermes_password_configured ? "密码已配置；留空保留" : "Dashboard 密码");
        Keyboard::Get().Bind(handles.hermes_password, "Dashboard 密码");
        if (callbacks.hermes_field_committed != nullptr) {
            lv_obj_add_event_cb(handles.hermes_password,
                                callbacks.hermes_field_committed, LV_EVENT_READY, nullptr);
        }

        handles.hermes_profile = CreateHermesInput(parent, "Agent / Profile");
        lv_textarea_set_one_line(handles.hermes_profile, true);
        lv_textarea_set_max_length(handles.hermes_profile, 128);
        lv_textarea_set_placeholder_text(handles.hermes_profile, "Agent/Profile，例如 default");
        lv_textarea_set_text(handles.hermes_profile,
            model.hermes_profile != nullptr ? model.hermes_profile : "");
        Keyboard::Get().Bind(handles.hermes_profile, "Hermes Agent/Profile");
        if (callbacks.hermes_field_committed != nullptr) {
            lv_obj_add_event_cb(handles.hermes_profile,
                                callbacks.hermes_field_committed, LV_EVENT_READY, nullptr);
        }

        const char* test_status =
            model.hermes_test_status != nullptr ? model.hermes_test_status : "";
        lv_obj_t* test_row = controls::CreateRow(
            parent, FONT_AWESOME_LINK, "测试连接", nullptr);
        lv_obj_add_flag(test_row, LV_OBJ_FLAG_CLICKABLE);
        if (callbacks.hermes_test != nullptr) {
            lv_obj_add_event_cb(test_row, callbacks.hermes_test, LV_EVENT_CLICKED, nullptr);
        }
        AttachButtonHaptic(test_row);
        handles.hermes_test_status = controls::AddValueLabel(
            test_row, test_status, 180);
        lv_obj_set_style_text_font(handles.hermes_test_status,
                                   fonts::MediumBold(), LV_PART_MAIN);

        lv_obj_t* save_row = controls::CreateRow(
            parent, FONT_AWESOME_PEN_TO_SQUARE, "应用 Hermes 配置", nullptr);
        lv_obj_add_flag(save_row, LV_OBJ_FLAG_CLICKABLE);
        if (callbacks.hermes_save != nullptr) {
            lv_obj_add_event_cb(save_row, callbacks.hermes_save, LV_EVENT_CLICKED, nullptr);
        }
        AttachButtonHaptic(save_row);
        const char* apply_status =
            model.hermes_apply_status != nullptr ? model.hermes_apply_status : "";
        handles.hermes_apply_status = controls::AddValueLabel(
            save_row, apply_status, 180);
        lv_obj_set_style_text_font(handles.hermes_apply_status,
                                   fonts::MediumBold(), LV_PART_MAIN);
    }
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
