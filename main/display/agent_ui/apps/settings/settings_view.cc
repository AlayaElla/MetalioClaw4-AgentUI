#include "settings_view.h"
#include "settings_panels_ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <vector>

#include <esp_app_desc.h>
#include <esp_log.h>
#include "application.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "components/ui_components.h"
#include "apps/bluetooth/bluetooth_module.h"
#include "apps/network/network_module.h"
#include "core/app_shell.h"
#include "core/fonts.h"
#include "core/idle_power.h"
#include "core/navigation.h"
#include "core/status_bar.h"
#include "core/theme.h"
#include "core/ui_utils.h"
#include "i18n.h"
#include "provisioning_client.h"
#include "settings.h"

namespace agent_ui {
namespace {

namespace controls = ui_components;
namespace panels_ui = settings_panels_ui;

network::Module s_network_module;
bluetooth::Module s_bluetooth_module;

constexpr lv_opa_t kAccentSoftOpacity = 0x1F;

enum class Panel : uint8_t { General, Ai, Network, Bluetooth, Language, About };
enum class EmbeddedView : uint8_t { None, Network, Bluetooth };

struct UiState {
    lv_obj_t* root = nullptr;
    lv_obj_t* panel = nullptr;
    std::array<lv_obj_t*, 6> tabs{};
    std::array<lv_obj_t*, 6> tab_labels{};
    std::array<lv_obj_t*, 6> tab_indicators{};
    Panel current = Panel::General;
    EmbeddedView embedded = EmbeddedView::None;
    lv_obj_t* brightness_value = nullptr;
    lv_obj_t* volume_value = nullptr;
    lv_obj_t* standby_value = nullptr;
    lv_obj_t* config_refresh_row = nullptr;
    lv_obj_t* config_refresh_status = nullptr;
};

UiState s_ui;
std::atomic<bool> s_config_refreshing{false};

constexpr char kTag[] = "AgentSettings";

constexpr std::array<const char*, 6> kTabLabels = {
    "常规", "AI", "网络", "蓝牙", "语言", "关于",
};

void BuildPanel(Panel panel);

void SetValueText(lv_obj_t* label, int value, const char* suffix) {
    if (label == nullptr) return;
    char text[24];
    std::snprintf(text, sizeof(text), "%d%s", value, suffix != nullptr ? suffix : "");
    lv_label_set_text(label, text);
}

int ReadBrightness() {
    if (auto* backlight = Board::GetInstance().GetBacklight()) {
        return std::clamp(static_cast<int>(backlight->brightness()),
                          static_cast<int>(kBacklightMinPercent), 100);
    }
    Settings settings("display", true);
    return static_cast<int>(std::clamp<int32_t>(
        settings.GetInt("brightness", kBacklightDefaultPercent),
        kBacklightMinPercent, 100));
}

int ReadVolume() {
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        return std::clamp(codec->output_volume(), 0, 100);
    }
    return 70;
}

void OnBrightnessChanged(lv_event_t* event) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const int value = lv_slider_get_value(slider);
    SetValueText(s_ui.brightness_value, value, "%");
    if (auto* backlight = Board::GetInstance().GetBacklight()) {
        backlight->SetBrightness(static_cast<uint8_t>(value), true);
    }
}

void OnVolumeChanged(lv_event_t* event) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const int value = lv_slider_get_value(slider);
    SetValueText(s_ui.volume_value, value, "%");
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        codec->SetOutputVolume(value);
    }
}

int StandbyIndexForMinutes(int minutes) {
    for (size_t i = 0; i < IdlePower::kStandbyMinuteOptions.size(); ++i) {
        if (minutes == IdlePower::kStandbyMinuteOptions[i]) {
            return static_cast<int>(i);
        }
    }
    return 1;  // The default five-minute option.
}

void OnStandbyMinutesChanged(lv_event_t* event) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const int raw_index = lv_slider_get_value(slider);
    const int index = std::clamp(raw_index, 0, 4);
    if (raw_index != index) {
        lv_slider_set_value(slider, index, LV_ANIM_OFF);
    }
    const int minutes = IdlePower::kStandbyMinuteOptions[index];
    IdlePower::Get().SetStandbyMinutes(minutes);
    SetValueText(s_ui.standby_value, minutes, " 分钟");
}

void OnAccentClicked(lv_event_t* event) {
    const auto preset = static_cast<AccentPreset>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    Theme::Get().SetAccentPreset(preset);
    StatusBar::Get().Refresh(true);
    Navigation::Get().RebuildCurrent();
}

void OnAppearanceClicked(lv_event_t* event) {
    const auto mode = static_cast<AppearanceMode>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    Theme::Get().SetAppearanceMode(mode);
    StatusBar::Get().Refresh(true);
    Navigation::Get().RebuildCurrent();
}

void BuildGeneralPanel() {
    const int brightness = ReadBrightness();
    const int volume = ReadVolume();
    const int standby_minutes = IdlePower::Get().standby_minutes();
    panels_ui::GeneralModel model{
        .appearance = static_cast<size_t>(Theme::Get().appearance_mode()),
        .accent = static_cast<size_t>(Theme::Get().accent_preset()),
        .brightness_min = kBacklightMinPercent,
        .brightness = brightness,
        .volume = volume,
        .standby_index = StandbyIndexForMinutes(standby_minutes),
    };
    panels_ui::GeneralCallbacks callbacks{
        .appearance = OnAppearanceClicked,
        .accent = OnAccentClicked,
        .brightness = OnBrightnessChanged,
        .volume = OnVolumeChanged,
        .standby = OnStandbyMinutesChanged,
    };
    const auto handles = panels_ui::BuildGeneral(s_ui.panel, model, callbacks);
    s_ui.brightness_value = handles.brightness_value;
    s_ui.volume_value = handles.volume_value;
    s_ui.standby_value = handles.standby_value;
    SetValueText(s_ui.brightness_value, brightness, "%");
    SetValueText(s_ui.volume_value, volume, "%");
    SetValueText(s_ui.standby_value, standby_minutes, " 分钟");
}

void OnWakeSwitchChanged(lv_event_t* event) {
    auto* control = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const bool enabled = lv_obj_has_state(control, LV_STATE_CHECKED);
    Settings settings("agent_ai", true);
    settings.SetInt("wake", enabled ? 1 : 0);
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(enabled);
}

void SetConfigRefreshUi(bool refreshing, const char* status, uint32_t color) {
    if (s_ui.config_refresh_row != nullptr) {
        refreshing ? lv_obj_clear_flag(s_ui.config_refresh_row, LV_OBJ_FLAG_CLICKABLE)
                   : lv_obj_add_flag(s_ui.config_refresh_row, LV_OBJ_FLAG_CLICKABLE);
    }
    if (s_ui.config_refresh_status != nullptr) {
        lv_label_set_text(s_ui.config_refresh_status, status != nullptr ? status : "");
        lv_obj_set_style_text_color(s_ui.config_refresh_status, lv_color_hex(color),
                                    LV_PART_MAIN);
    }
}

struct ConfigRefreshResult {
    esp_err_t error = ESP_FAIL;
};

void ApplyConfigRefreshResult(void* user_data) {
    auto* result = static_cast<ConfigRefreshResult*>(user_data);
    const esp_err_t error = result != nullptr ? result->error : ESP_FAIL;
    delete result;
    s_config_refreshing.store(false);

    const auto& colors = Theme::Get().colors();
    if (error == ESP_OK) {
        SetConfigRefreshUi(false, "配置已更新", colors.accent);
    } else {
        SetConfigRefreshUi(false, "获取失败", colors.danger);
    }
}

void ConfigRefreshTask(void*) {
    ProvisioningClient client;
    auto* result = new ConfigRefreshResult{client.FetchConfiguration()};
    lv_async_call(ApplyConfigRefreshResult, result);
    vTaskDelete(nullptr);
}

void OnConfigRefreshClicked(lv_event_t*) {
    bool expected = false;
    if (!s_config_refreshing.compare_exchange_strong(expected, true)) return;

    SetConfigRefreshUi(true, "正在获取", Theme::Get().colors().muted);
    if (xTaskCreate(ConfigRefreshTask, "agent_config", 8192, nullptr,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Failed to start provisioning refresh task");
        s_config_refreshing.store(false);
        SetConfigRefreshUi(false, "系统忙", Theme::Get().colors().danger);
    }
}

void BuildAiPanel() {
    Settings settings("agent_ai", true);
    const bool refreshing = s_config_refreshing.load();
    const auto handles = panels_ui::BuildAi(
        s_ui.panel,
        panels_ui::AiModel{
            .wake_enabled = settings.GetInt("wake", 1) != 0,
            .config_refreshing = refreshing,
        },
        panels_ui::AiCallbacks{
            .wake_changed = OnWakeSwitchChanged,
            .config_refresh = OnConfigRefreshClicked,
        });
    s_ui.config_refresh_row = handles.config_row;
    s_ui.config_refresh_status = handles.config_status;
    if (refreshing) {
        SetConfigRefreshUi(true, "正在获取", Theme::Get().colors().muted);
    }
}

void DeactivateEmbeddedView() {
    switch (s_ui.embedded) {
        case EmbeddedView::Network:
            s_network_module.LifecycleCallback(AppLifecycleEvent::Unload);
            s_network_module.ResetUi();
            break;
        case EmbeddedView::Bluetooth:
            s_bluetooth_module.LifecycleCallback(AppLifecycleEvent::Unload);
            s_bluetooth_module.ResetUi();
            break;
        case EmbeddedView::None:
            break;
    }
    s_ui.embedded = EmbeddedView::None;
}

void BuildNetworkPanel() {
    s_network_module.BuildInto(s_ui.panel);
    s_network_module.LifecycleCallback(AppLifecycleEvent::Load);
    s_ui.embedded = EmbeddedView::Network;
}

void BuildBluetoothPanel() {
    s_bluetooth_module.BuildInto(s_ui.panel);
    s_bluetooth_module.LifecycleCallback(AppLifecycleEvent::Load);
    s_ui.embedded = EmbeddedView::Bluetooth;
}

void OnLanguageClicked(lv_event_t* event) {
    const auto locale = static_cast<I18n::Locale>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (I18n::SetLocale(locale)) Navigation::Get().RebuildCurrent();
}

void BuildLanguagePanel() {
    const I18n::Locale current = I18n::GetLocale();
    std::vector<panels_ui::LanguageOption> options;
    options.reserve(I18n::GetLocaleCount());
    for (size_t i = 0; i < I18n::GetLocaleCount(); ++i) {
        const auto locale = static_cast<I18n::Locale>(i);
        const auto* info = I18n::GetLocaleInfo(locale);
        if (info == nullptr) continue;
        options.push_back({
            .label = info->native_name,
            .id = i,
            .selected = locale == current,
        });
    }
    panels_ui::BuildLanguage(s_ui.panel, options.data(), options.size(),
                             OnLanguageClicked);
}

void BuildAboutPanel() {
    const esp_app_desc_t* description = esp_app_get_description();
    panels_ui::BuildAbout(
        s_ui.panel,
        panels_ui::AboutInfo{
            .product = "AgentUI",
            .version = description != nullptr ? description->version : "--",
            .device = "MetalioClaw4 · ESP32-P4",
            .display = "720 × 720",
        });
}

void UpdateTabStyles() {
    const auto& colors = Theme::Get().colors();
    for (size_t i = 0; i < s_ui.tabs.size(); ++i) {
        lv_obj_t* tab = s_ui.tabs[i];
        if (tab == nullptr) continue;
        const bool selected = static_cast<size_t>(s_ui.current) == i;
        lv_obj_set_style_bg_color(
            tab, lv_color_hex(selected ? colors.accent : colors.raised),
            LV_PART_MAIN);
        const lv_opa_t background_opacity =
            selected ? kAccentSoftOpacity : static_cast<lv_opa_t>(LV_OPA_COVER);
        lv_obj_set_style_bg_opa(tab, background_opacity, LV_PART_MAIN);
        lv_obj_t* label = s_ui.tab_labels[i];
        if (label != nullptr) {
            lv_obj_set_style_text_color(
                label, lv_color_hex(selected ? colors.accent : colors.muted),
                LV_PART_MAIN);
        }
        lv_obj_t* indicator = s_ui.tab_indicators[i];
        if (indicator != nullptr) {
            lv_obj_set_style_bg_opa(indicator,
                                    selected ? LV_OPA_COVER : LV_OPA_TRANSP,
                                    LV_PART_MAIN);
        }
    }
}

void OnTabClicked(lv_event_t* event) {
    BuildPanel(static_cast<Panel>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

void BuildPanel(Panel panel) {
    if (s_ui.panel == nullptr ||
        (panel == s_ui.current && lv_obj_get_child_count(s_ui.panel) != 0)) {
        return;
    }
    s_ui.current = panel;
    s_ui.brightness_value = nullptr;
    s_ui.volume_value = nullptr;
    s_ui.standby_value = nullptr;
    s_ui.config_refresh_row = nullptr;
    s_ui.config_refresh_status = nullptr;
    DeactivateEmbeddedView();
    lv_obj_clean(s_ui.panel);
    UpdateTabStyles();
    switch (panel) {
        case Panel::General: BuildGeneralPanel(); break;
        case Panel::Ai: BuildAiPanel(); break;
        case Panel::Network: BuildNetworkPanel(); break;
        case Panel::Bluetooth: BuildBluetoothPanel(); break;
        case Panel::Language: BuildLanguagePanel(); break;
        case Panel::About: BuildAboutPanel(); break;
    }
    lv_obj_scroll_to_y(s_ui.panel, 0, LV_ANIM_OFF);
}

void OnDeleted(lv_event_t* event) {
    // RebuildCurrent creates the replacement screen before LVGL deletes the
    // old one. Do not let that delayed delete clear the replacement handles.
    if (lv_event_get_target_obj(event) != s_ui.root) return;
    DeactivateEmbeddedView();
    s_ui = {};
}

void SettingsLifecycleCallback(AppLifecycleEvent event) {
    if (event != AppLifecycleEvent::Suspend &&
        event != AppLifecycleEvent::Resume) {
        return;
    }
    switch (s_ui.embedded) {
        case EmbeddedView::Network:
            s_network_module.LifecycleCallback(event);
            break;
        case EmbeddedView::Bluetooth:
            s_bluetooth_module.LifecycleCallback(event);
            break;
        case EmbeddedView::None:
            break;
    }
}

}  // namespace

lv_obj_t* SettingsView::Create() {
    auto shell = CreateAppShell("设置", nullptr);
    s_ui.root = shell.root;
    s_ui.current = Panel::General;
    lv_obj_add_event_cb(shell.root, OnDeleted, LV_EVENT_DELETE, nullptr);
    AttachAppLifecycle(shell.root, SettingsLifecycleCallback);

    const auto& colors = Theme::Get().colors();
    lv_obj_t* tab_bar = lv_obj_create(shell.actions);
    lv_obj_remove_style_all(tab_bar);
    lv_obj_set_height(tab_bar, metrics::kBottomActionHeight);
    lv_obj_set_flex_grow(tab_bar, 1);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(colors.raised), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(tab_bar, metrics::kRadiusControl, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(tab_bar, true, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(tab_bar, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < s_ui.tabs.size(); ++i) {
        lv_obj_t* tab = controls::CreateButton(tab_bar);
        lv_obj_remove_style_all(tab);
        lv_obj_set_height(tab, metrics::kBottomActionHeight);
        lv_obj_set_flex_grow(tab, 1);
        lv_obj_set_style_radius(tab, 12, LV_PART_MAIN);
        lv_obj_add_event_cb(tab, OnTabClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(i));

        lv_obj_t* label = lv_label_create(tab);
        lv_label_set_text(label, kTabLabels[i]);
        lv_obj_set_style_text_font(label, fonts::MediumBold(), LV_PART_MAIN);
        lv_obj_center(label);

        lv_obj_t* indicator = lv_obj_create(tab);
        lv_obj_remove_style_all(indicator);
        lv_obj_set_size(indicator, LV_PCT(68), 4);
        lv_obj_align(indicator, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(indicator, lv_color_hex(colors.accent),
                                 LV_PART_MAIN);
        lv_obj_set_style_bg_opa(indicator, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
        s_ui.tabs[i] = tab;
        s_ui.tab_labels[i] = label;
        s_ui.tab_indicators[i] = indicator;
    }

    s_ui.panel = lv_obj_create(shell.content);
    controls::StylePanel(s_ui.panel);
    lv_obj_set_size(s_ui.panel, metrics::kDisplaySize,
                    metrics::kBottomActionContentHeight);
    lv_obj_set_pos(s_ui.panel, 0, 0);

    // Force the initial render even though General is the default enum value.
    lv_obj_clean(s_ui.panel);
    UpdateTabStyles();
    BuildGeneralPanel();
    return shell.root;
}

}  // namespace agent_ui
