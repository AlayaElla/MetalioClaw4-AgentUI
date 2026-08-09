#include "idle_power.h"

#include <esp_log.h>

#include "application.h"
#include "board.h"
#include "settings.h"
#include "apps/home/home_renderer.h"
#include "apps/standby/standby_view.h"
#include "core/navigation.h"
#include "core/performance_manager.h"

namespace agent_ui {
namespace {
constexpr char kTag[] = "AgentIdlePower";
constexpr char kStandbyKey[] = "idle_stby_min";
constexpr uint32_t kBatteryRefreshMs = 30000;
}

IdlePower& IdlePower::Get() {
    static IdlePower instance;
    return instance;
}

void IdlePower::Initialize(Board& board) {
    if (timer_ != nullptr) return;
    standby_minutes_ = standby_minutes();
    PerformanceManager::Get().Initialize(board);
    NotifyActivity();
    timer_ = lv_timer_create(TimerCallback, 1000, this);
}

void IdlePower::NotifyActivity() {
    if (StandbyView::IsActive()) return;
    last_activity_tick_ = lv_tick_get();
    expression_sleep_triggered_ = false;
    PerformanceManager::Get().NotifyActivity();
    home::Renderer::NotifyUserActivity();
}

void IdlePower::RestoreExpressionSleep() {
    expression_sleep_triggered_ = true;
    home::Renderer::SleepExpression();
}

void IdlePower::SetStandbyActive(bool active) {
    standby_active_ = active;
    if (active) {
        PerformanceManager::Get().SetStandbyPhase(
            StandbyPerformancePhase::Dim);
    } else {
        PerformanceManager::Get().SetStandbyPhase(
            StandbyPerformancePhase::Awake);
        NotifyActivity();
    }
}

int IdlePower::standby_minutes() const {
    if (standby_minutes_ >= 0) return standby_minutes_;
    Settings settings("display", false);
    const int32_t minutes =
        settings.GetInt(kStandbyKey, kDefaultStandbyMinutes);
    return NormalizeStandbyMinutes(static_cast<int>(minutes));
}

void IdlePower::SetStandbyMinutes(int minutes) {
    minutes = NormalizeStandbyMinutes(minutes);
    Settings settings("display", true);
    settings.SetInt(kStandbyKey, minutes);
    standby_minutes_ = minutes;
    NotifyActivity();
    ESP_LOGI(kTag, "Standby timeout updated to %d minutes", minutes);
}

int IdlePower::NormalizeStandbyMinutes(int minutes) {
    for (const int option : kStandbyMinuteOptions) {
        if (minutes == option) return option;
    }
    // Older builds accepted arbitrary values (including 0 to disable the
    // timer). Keep valid existing values, but migrate unsupported settings to
    // the documented default instead of retaining an unreachable mode.
    return kDefaultStandbyMinutes;
}

void IdlePower::TimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<IdlePower*>(lv_timer_get_user_data(timer));
    if (self != nullptr) self->Tick();
}

void IdlePower::Tick() {
    if (standby_active_) return;

    const uint32_t now = lv_tick_get();
    auto& application = Application::GetInstance();
    const DeviceState device_state = application.GetDeviceState();
    // Idle wake-word detection is deliberately not a high-performance demand.
    // Active listening and Codex capture need stable real-time audio, but can
    // use the balanced tier after their short transition boost expires.
    const bool foreground_ai_busy =
        device_state == kDeviceStateConnecting ||
        device_state == kDeviceStateSpeaking ||
        device_state == kDeviceStateAudioTesting;
    const bool realtime_audio_busy =
        device_state == kDeviceStateListening ||
        application.IsCodexVoiceCaptureActive();
    const bool camera_busy =
        Navigation::Get().current() == ScreenId::Camera;
    auto& performance = PerformanceManager::Get();
    performance.SetDemand(PerformanceDemand::Ai, foreground_ai_busy);
    performance.SetDemand(PerformanceDemand::RealtimeAudio,
                          realtime_audio_busy);
    performance.SetDemand(PerformanceDemand::Camera, camera_busy);
    performance.Tick();

    if (home::Renderer::IsMounted() &&
        (last_battery_refresh_tick_ == 0 ||
         lv_tick_elaps(last_battery_refresh_tick_) >= kBatteryRefreshMs)) {
        last_battery_refresh_tick_ = now;
        int battery_level = 0;
        bool charging = false;
        bool discharging = false;
        const bool has_battery = Board::GetInstance().GetBatteryLevel(
            battery_level, charging, discharging);
        home::Renderer::UpdateBattery(has_battery, battery_level, charging);
    }

    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            NotifyActivity();
            return;
        }
    }

    const int minutes = standby_minutes();
    const uint32_t idle_ms = lv_tick_elaps(last_activity_tick_);
    const uint32_t standby_timeout_ms =
        static_cast<uint32_t>(minutes) * 60U * 1000U;
    const uint32_t expression_sleep_ms =
        standby_timeout_ms > 1000U ? standby_timeout_ms - 1000U
                                   : standby_timeout_ms;
    if (standby_timeout_ms > 0 && idle_ms >= standby_timeout_ms) {
        standby_active_ = true;
        ESP_LOGI(kTag, "Entering standby after %d idle minutes", minutes);
        StandbyView::Show();
        return;
    }

    // Keep the expression sleep transition on the same user-selected clock
    // as standby. It runs one timer tick before the standby screen so the
    // sleep animation can begin without delaying the configured timeout.
    if (standby_timeout_ms > 0 && idle_ms >= expression_sleep_ms &&
        !expression_sleep_triggered_ &&
        Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        expression_sleep_triggered_ = true;
        home::Renderer::SleepExpression();
        Application::GetInstance().TriggerSpecialInteraction(
            SpecialInteraction::Sleep);
        return;
    }
}

}  // namespace agent_ui
