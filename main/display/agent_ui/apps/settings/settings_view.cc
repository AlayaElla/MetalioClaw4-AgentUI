#include "settings_view.h"
#include "settings_panels_ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <esp_app_desc.h>
#include <esp_log.h>
#include "ai_provider_config.h"
#include "application.h"
#include "hermes_voice_session.h"
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
    lv_obj_t* hermes_dashboard_url = nullptr;
    lv_obj_t* hermes_username = nullptr;
    lv_obj_t* hermes_password = nullptr;
    lv_obj_t* hermes_profile = nullptr;
    lv_obj_t* hermes_test_status = nullptr;
    lv_obj_t* hermes_apply_status = nullptr;
    AiProviderConfig ai_config;
};

UiState s_ui;
std::string s_hermes_test_status;
std::string s_hermes_apply_status;
std::atomic<bool> s_hermes_test_active{false};
std::atomic<bool> s_hermes_save_active{false};
std::atomic<uint32_t> s_ui_generation{0};
std::atomic<uint32_t> s_hermes_test_epoch{0};

constexpr char kTag[] = "AgentSettings";

constexpr std::array<const char*, 6> kTabLabels = {
    "常规", "AI", "网络", "蓝牙", "语言", "关于",
};

void BuildPanel(Panel panel);
void BuildAiPanel();

void RebuildAiPanel() {
    if (s_ui.current != Panel::Ai || s_ui.panel == nullptr) return;
    const int32_t scroll_y = lv_obj_get_scroll_y(s_ui.panel);
    s_ui.hermes_dashboard_url = nullptr;
    s_ui.hermes_username = nullptr;
    s_ui.hermes_password = nullptr;
    s_ui.hermes_profile = nullptr;
    s_ui.hermes_test_status = nullptr;
    s_ui.hermes_apply_status = nullptr;
    lv_obj_clean(s_ui.panel);
    BuildAiPanel();
    // Status changes and async task results rebuild this panel. Recalculate
    // the new content height before restoring the user's viewport so action
    // buttons do not send the Hermes settings back to the top.
    lv_obj_update_layout(s_ui.panel);
    lv_obj_scroll_to_y(s_ui.panel, scroll_y, LV_ANIM_OFF);
}

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
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(enabled);
    // A tiny synchronous NVS write preserves toggle ordering.
    Settings settings(std::string(ai_provider_config::kNamespace), true);
    settings.SetInt("wake", enabled ? 1 : 0);
}

struct HermesTaskResult {
    std::string error;
    AiProviderConfig config;
    bool success = false;
    uint32_t ui_generation = 0;
    uint32_t request_epoch = 0;
};

struct HermesTestRequest {
    AiProviderConfig config;
    uint32_t ui_generation = 0;
    uint32_t request_epoch = 0;
};

struct HermesSaveRequest {
    AiProviderConfig config;
    uint32_t ui_generation = 0;
};

std::string ReadTextarea(lv_obj_t* textarea) {
    const char* text = textarea != nullptr ? lv_textarea_get_text(textarea) : nullptr;
    return text != nullptr ? text : "";
}

AiProviderConfig DraftHermesConfig() {
    AiProviderConfig config = s_ui.ai_config;
    config.provider = AiProvider::Hermes;
    config.hermes_dashboard_url = ai_provider_config::ResolveHermesBaseUrl(
        ReadTextarea(s_ui.hermes_dashboard_url));
    config.hermes_username = ReadTextarea(s_ui.hermes_username);
    const std::string password = ReadTextarea(s_ui.hermes_password);
    if (!password.empty()) config.hermes_password = password;
    config.hermes_profile = ReadTextarea(s_ui.hermes_profile);
    return config;
}

bool SameHermesConfig(const AiProviderConfig& left, const AiProviderConfig& right) {
    return left.provider == right.provider &&
        left.hermes_dashboard_url == right.hermes_dashboard_url &&
        left.hermes_username == right.hermes_username &&
        left.hermes_password == right.hermes_password &&
        left.hermes_profile == right.hermes_profile;
}

enum class HermesAction : uint8_t { Test, Apply };

void SetHermesActionStatus(HermesAction action, const char* status) {
    std::string& value = action == HermesAction::Test
        ? s_hermes_test_status : s_hermes_apply_status;
    lv_obj_t* label = action == HermesAction::Test
        ? s_ui.hermes_test_status : s_ui.hermes_apply_status;
    value = status != nullptr ? status : "";
    if (label != nullptr) lv_label_set_text(label, value.c_str());
}

bool ValidateHermesRequest(const AiProviderConfig& config, HermesAction action) {
    const char* error = nullptr;
    if (!ai_provider_config::IsValidHermesBaseUrl(config.hermes_dashboard_url)) {
        error = "Hermes 服务地址无效";
    } else if (config.hermes_username.empty() || config.hermes_password.empty()) {
        error = "Dashboard 用户名或密码为空";
    } else if (!ai_provider_config::IsValidHermesUsername(config.hermes_username) ||
               !ai_provider_config::IsValidHermesPassword(config.hermes_password)) {
        error = "Dashboard 用户名或密码格式无效";
    } else if (!ai_provider_config::IsValidHermesProfile(config.hermes_profile)) {
        error = "Agent/Profile 名称无效";
    }
    if (error == nullptr) return true;
    ESP_LOGW(kTag, "%s", error);
    SetHermesActionStatus(action, "配置有误");
    return false;
}

void SetHermesApplyValidationError(const char* error) {
    ESP_LOGW(kTag, "%s", error);
    SetHermesActionStatus(HermesAction::Apply, "配置有误");
}

void ClearStaleHermesTestStatus() {
    SetHermesActionStatus(HermesAction::Test, "");
}

void ApplyCompletedHermesDraft(const AiProviderConfig& draft) {
    Application::GetInstance().ApplyAiProviderSelection(draft);
    SetHermesActionStatus(HermesAction::Apply, "已应用");
}

void SaveIncompleteHermesDraft(const AiProviderConfig& draft) {
    Application::GetInstance().Schedule([draft]() {
        ai_provider_config::SaveHermesDraft(draft);
    });
    SetHermesActionStatus(HermesAction::Apply, "");
}

void CommitHermesDraft(const AiProviderConfig& draft) {
    ClearStaleHermesTestStatus();
    if (ai_provider_config::IsCompleteHermesConfig(draft)) {
        ApplyCompletedHermesDraft(draft);
    } else {
        SaveIncompleteHermesDraft(draft);
    }
}

void LogHermesTestFailure(const std::string& error) {
    ESP_LOGW(kTag, "Hermes connection test failed: %s",
             error.empty() ? "unknown error" : error.c_str());
}

void ApplyHermesTaskResult(void* user_data) {
    std::unique_ptr<HermesTaskResult> result(static_cast<HermesTaskResult*>(user_data));
    if (result == nullptr || result->ui_generation != s_ui_generation.load() ||
        result->request_epoch != s_hermes_test_epoch.load()) {
        return;
    }
    s_hermes_test_active.store(false);
    if (!SameHermesConfig(DraftHermesConfig(), result->config)) {
        ClearStaleHermesTestStatus();
        return;
    }
    if (!result->success) LogHermesTestFailure(result->error);
    SetHermesActionStatus(HermesAction::Test,
                          result->success ? "测试成功" : "测试失败");
}

void RunHermesConnectionTestTask(void* user_data) {
    std::unique_ptr<HermesTestRequest> request(
        static_cast<HermesTestRequest*>(user_data));
    auto* result = new HermesTaskResult;
    result->ui_generation = request->ui_generation;
    result->request_epoch = request->request_epoch;
    result->config = request->config;
    ai_provider_config::SaveHermesDraft(request->config);

    hermes_voice::DashboardSession session;
    std::vector<std::string> names;
    bool ok = hermes_voice::LoginDashboard(request->config, &session, &result->error) &&
        hermes_voice::CheckDashboardIdentity(request->config, session, &result->error) &&
        hermes_voice::LoadDashboardProfiles(request->config, session, &names, &result->error);
    if (ok && std::find(names.begin(), names.end(), request->config.hermes_profile) == names.end()) {
        ok = false;
        result->error = "未找到所选 Agent/Profile";
    }
    if (ok) {
        ok = hermes_voice::TestDashboardGateway(request->config, session, &result->error);
    }
    result->success = ok;
    lv_async_call(ApplyHermesTaskResult, result);
    vTaskDelete(nullptr);
}

void OnHermesTestClicked(lv_event_t*) {
    if (Application::GetInstance().IsHermesVoiceBusy()) {
        SetHermesActionStatus(HermesAction::Test, "请结束对话");
        return;
    }
    AiProviderConfig draft = DraftHermesConfig();
    if (!ValidateHermesRequest(draft, HermesAction::Test)) return;
    s_ui.ai_config = draft;
    bool expected = false;
    if (!s_hermes_test_active.compare_exchange_strong(expected, true)) return;
    SetHermesActionStatus(HermesAction::Test, "测试中");
    const uint32_t request_epoch =
        s_hermes_test_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* request = new HermesTestRequest{
        .config = std::move(draft),
        .ui_generation = s_ui_generation.load(),
        .request_epoch = request_epoch,
    };
    if (xTaskCreate(RunHermesConnectionTestTask, "hermes_test", 8192, request,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        delete request;
        s_hermes_test_active.store(false);
        SetHermesActionStatus(HermesAction::Test, "测试失败");
    }
}

void OnHermesFieldCommitted(lv_event_t* event) {
    lv_obj_t* field = lv_event_get_target_obj(event);
    AiProviderConfig draft = DraftHermesConfig();
    const std::string entered_password = ReadTextarea(s_ui.hermes_password);
    if (field == s_ui.hermes_dashboard_url &&
        !ai_provider_config::IsValidHermesBaseUrl(draft.hermes_dashboard_url)) {
        SetHermesApplyValidationError("Hermes 服务地址无效，未保存");
        return;
    }
    if (field == s_ui.hermes_username &&
        !ai_provider_config::IsValidHermesUsername(draft.hermes_username)) {
        SetHermesApplyValidationError("Dashboard 用户名无效，未保存");
        return;
    }
    if (field == s_ui.hermes_password && !entered_password.empty() &&
        !ai_provider_config::IsValidHermesPassword(entered_password)) {
        SetHermesApplyValidationError("Dashboard 密码无效，未保存");
        return;
    }
    if (field == s_ui.hermes_profile &&
        !ai_provider_config::IsValidHermesProfile(draft.hermes_profile)) {
        SetHermesApplyValidationError("Agent/Profile 名称无效，未保存");
        return;
    }

    s_ui.ai_config = draft;
    CommitHermesDraft(draft);
}

void OnAiProviderChanged(lv_event_t* event) {
    const bool hermes = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) != 0;
    if (!hermes) {
        s_ui.ai_config.provider = AiProvider::Xiaozhi;
        Application::GetInstance().ApplyAiProviderSelection(s_ui.ai_config);
    } else {
        s_ui.ai_config.provider = AiProvider::Hermes;
    }
    RebuildAiPanel();
}

void PersistHermesDraftTask(void* user_data);

void OnHermesSaveClicked(lv_event_t*) {
    AiProviderConfig draft = DraftHermesConfig();
    if (!ValidateHermesRequest(draft, HermesAction::Apply)) return;
    bool expected = false;
    if (!s_hermes_save_active.compare_exchange_strong(expected, true)) {
        SetHermesActionStatus(HermesAction::Apply, "应用中");
        return;
    }
    auto* request = new HermesSaveRequest{
        .config = draft,
        .ui_generation = s_ui_generation.load(),
    };
    if (xTaskCreate(PersistHermesDraftTask, "hermes_save", 4096, request,
                    tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        delete request;
        s_hermes_save_active.store(false);
        SetHermesActionStatus(HermesAction::Apply, "应用失败");
    } else {
        s_ui.ai_config = std::move(draft);
        ClearStaleHermesTestStatus();
        SetHermesActionStatus(HermesAction::Apply, "应用中");
    }
}

void ApplyHermesSaveQueued(void* user_data) {
    std::unique_ptr<uint32_t> generation(static_cast<uint32_t*>(user_data));
    s_hermes_save_active.store(false);
    if (generation != nullptr && *generation == s_ui_generation.load()) {
        SetHermesActionStatus(HermesAction::Apply, "已应用");
    }
}

void PersistHermesDraftTask(void* user_data) {
    std::unique_ptr<HermesSaveRequest> request(
        static_cast<HermesSaveRequest*>(user_data));
    Application::GetInstance().ApplyAiProviderSelection(request->config);
    lv_async_call(ApplyHermesSaveQueued, new uint32_t(request->ui_generation));
    vTaskDelete(nullptr);
}

void BuildAiPanel() {
    const auto handles = panels_ui::BuildAi(
        s_ui.panel,
        panels_ui::AiModel{
            .wake_enabled = Settings(std::string(ai_provider_config::kNamespace), false)
                                .GetInt("wake", 1) != 0,
            .hermes_selected = s_ui.ai_config.provider == AiProvider::Hermes,
            .hermes_dashboard_url = s_ui.ai_config.hermes_dashboard_url.c_str(),
            .hermes_username = s_ui.ai_config.hermes_username.c_str(),
            .hermes_password_configured =
                ai_provider_config::IsValidHermesPassword(s_ui.ai_config.hermes_password),
            .hermes_profile = s_ui.ai_config.hermes_profile.c_str(),
            .hermes_test_status = s_hermes_test_status.c_str(),
            .hermes_apply_status = s_hermes_apply_status.c_str(),
        },
        panels_ui::AiCallbacks{
            .wake_changed = OnWakeSwitchChanged,
            .provider_changed = OnAiProviderChanged,
            .hermes_field_committed = OnHermesFieldCommitted,
            .hermes_save = OnHermesSaveClicked,
            .hermes_test = OnHermesTestClicked,
        });
    s_ui.hermes_dashboard_url = handles.hermes_dashboard_url;
    s_ui.hermes_username = handles.hermes_username;
    s_ui.hermes_password = handles.hermes_password;
    s_ui.hermes_profile = handles.hermes_profile;
    s_ui.hermes_test_status = handles.hermes_test_status;
    s_ui.hermes_apply_status = handles.hermes_apply_status;
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
    s_ui.hermes_dashboard_url = nullptr;
    s_ui.hermes_username = nullptr;
    s_ui.hermes_password = nullptr;
    s_ui.hermes_profile = nullptr;
    s_ui.hermes_test_status = nullptr;
    s_ui.hermes_apply_status = nullptr;
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
    s_ui_generation.fetch_add(1, std::memory_order_acq_rel);
    s_hermes_test_epoch.fetch_add(1, std::memory_order_acq_rel);
    s_hermes_test_active.store(false);
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
    s_ui_generation.fetch_add(1, std::memory_order_acq_rel);
    s_hermes_test_epoch.fetch_add(1, std::memory_order_acq_rel);
    s_hermes_test_active.store(false);
    s_hermes_test_status.clear();
    s_hermes_apply_status.clear();
    auto shell = CreateAppShell("设置", nullptr);
    s_ui.root = shell.root;
    s_ui.current = Panel::General;
    s_ui.ai_config = ai_provider_config::Load();
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
