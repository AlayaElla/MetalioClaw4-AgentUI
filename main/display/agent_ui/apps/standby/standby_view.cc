#include "standby_view.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>

#include <esp_log.h>
#include <font_awesome.h>

#include "application.h"
#include "audio_output_route.h"
#include "apps/home/home_renderer.h"
#include "backlight.h"
#include "board.h"
#include "components/expression_player.h"
#include "core/fonts.h"
#include "core/idle_power.h"
#include "core/performance_manager.h"
#include "core/power_key.h"
#include "core/status_bar.h"
#include "core/theme.h"
#include "core/ui_utils.h"
#include "display.h"

namespace agent_ui {
namespace {

constexpr char kTag[] = "AgentStandby";
constexpr uint32_t kScreenOffDelayMs = 10000;
constexpr uint8_t kDimBrightnessPercent = 5;
constexpr int kSliderX = 30;
constexpr int kSliderY = 602;
constexpr int kSliderWidth = 660;
constexpr int kSliderHeight = metrics::kBottomPrimaryActionHeight;
constexpr int kSliderInset = 4;
constexpr int kSliderThumbSize = 96;
constexpr int kSliderRange =
    kSliderWidth - kSliderThumbSize - kSliderInset * 2;
constexpr int kUnlockThreshold =
    kSliderRange * 86 / 100;

struct State {
    lv_obj_t* source_screen = nullptr;
    lv_obj_t* lock_overlay = nullptr;
    lv_obj_t* black_overlay = nullptr;
    lv_obj_t* time_label = nullptr;
    lv_obj_t* date_label = nullptr;
    lv_obj_t* slider_fill = nullptr;
    lv_obj_t* slider_label = nullptr;
    lv_obj_t* slider_thumb = nullptr;
    ExpressionPlayer* expression = nullptr;
    lv_timer_t* clock_timer = nullptr;
    lv_timer_t* off_timer = nullptr;
    int drag_start_x = 0;
    int drag_offset = 0;
    bool active = false;
    bool black = false;
    bool dragging = false;
    bool invalidation_suspended = false;
    bool peripherals_pending = false;
    bool peripherals_failed = false;
    bool audio_ready = false;
    bool sleep_after_wake = false;
    bool immediate_sleep_after_wake = false;
};

State s_ui;
std::atomic<bool> s_active_snapshot{false};
std::atomic<bool> s_screen_off_snapshot{false};

void ScheduleScreenOff();
void UnlockToSource();

void StartPeripheralWake() {
    s_ui.peripherals_pending = true;
    s_ui.peripherals_failed = false;
    s_ui.audio_ready = false;
    if (!PowerKey::ResumeStandbyPeripherals()) {
        s_ui.peripherals_pending = false;
        s_ui.peripherals_failed = true;
    }
    if (s_ui.slider_label != nullptr) {
        lv_label_set_text(s_ui.slider_label, "向右滑动解锁");
    }
}

void DeleteTimer(lv_timer_t*& timer) {
    if (timer == nullptr) return;
    lv_timer_delete(timer);
    timer = nullptr;
}

void SuspendDisplayInvalidation() {
    if (s_ui.invalidation_suspended) return;
    lv_display_enable_invalidation(nullptr, false);
    s_ui.invalidation_suspended = true;
}

void ResumeDisplayInvalidation() {
    if (!s_ui.invalidation_suspended) return;
    lv_display_enable_invalidation(nullptr, true);
    s_ui.invalidation_suspended = false;
}

void UpdateClock(lv_timer_t*) {
    if (!s_ui.active || s_ui.time_label == nullptr ||
        s_ui.date_label == nullptr) {
        return;
    }

    char time_text[8] = "--:--";
    char date_text[48] = "--月--日";
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    if (localtime_r(&now, &local) != nullptr && local.tm_year >= 125) {
        static constexpr const char* kWeekdays[] = {
            "星期日", "星期一", "星期二", "星期三",
            "星期四", "星期五", "星期六",
        };
        std::strftime(time_text, sizeof(time_text), "%H:%M", &local);
        std::snprintf(date_text, sizeof(date_text), "%d月%d日 %s",
                      local.tm_mon + 1, local.tm_mday,
                      kWeekdays[local.tm_wday]);
    }
    lv_label_set_text(s_ui.time_label, time_text);
    lv_label_set_text(s_ui.date_label, date_text);
}

void SetSliderOffset(int offset) {
    if (s_ui.slider_thumb == nullptr || s_ui.slider_fill == nullptr ||
        s_ui.slider_label == nullptr) {
        return;
    }
    s_ui.drag_offset = std::clamp(offset, 0, kSliderRange);
    lv_obj_set_x(s_ui.slider_thumb, kSliderInset + s_ui.drag_offset);
    lv_obj_set_width(s_ui.slider_fill, kSliderHeight + s_ui.drag_offset);
    const bool ready = s_ui.drag_offset >= kUnlockThreshold;
    lv_label_set_text(s_ui.slider_label,
                      ready ? "松手解锁" : "向右滑动解锁");
    lv_obj_set_style_text_color(
        s_ui.slider_label,
        lv_color_hex(ready ? Theme::Get().colors().accent
                           : Theme::Get().colors().muted),
        LV_PART_MAIN);
}

void ResetSlider() {
    s_ui.dragging = false;
    SetSliderOffset(0);
}

void OnSliderPressed(lv_event_t* event) {
    if (!s_ui.active || s_ui.black) return;
    lv_indev_t* indev = lv_event_get_indev(event);
    if (indev == nullptr) indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    s_ui.drag_start_x = point.x - s_ui.drag_offset;
    s_ui.dragging = true;
    ScheduleScreenOff();
}

void OnSliderPressing(lv_event_t* event) {
    if (!s_ui.dragging || s_ui.black) return;
    lv_indev_t* indev = lv_event_get_indev(event);
    if (indev == nullptr) indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    SetSliderOffset(point.x - s_ui.drag_start_x);
}

void OnSliderReleased(lv_event_t* event) {
    if (!s_ui.dragging) return;
    s_ui.dragging = false;
    const bool completed =
        lv_event_get_code(event) == LV_EVENT_RELEASED &&
        s_ui.drag_offset >= kUnlockThreshold;
    if (completed) {
        UnlockToSource();
    } else {
        SetSliderOffset(0);
    }
}

void CreateLockUi() {
    const auto& colors = Theme::Get().colors();
    s_ui.lock_overlay = lv_obj_create(s_ui.source_screen);
    lv_obj_remove_style_all(s_ui.lock_overlay);
    lv_obj_set_size(s_ui.lock_overlay, metrics::kDisplaySize,
                    metrics::kDisplaySize);
    lv_obj_set_pos(s_ui.lock_overlay, 0, 0);
    lv_obj_set_style_bg_color(
        s_ui.lock_overlay, lv_color_hex(colors.background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.lock_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.lock_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.lock_overlay, LV_OBJ_FLAG_CLICKABLE);

    s_ui.time_label = lv_label_create(s_ui.lock_overlay);
    lv_obj_set_style_text_font(s_ui.time_label, fonts::LargeBold(),
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.time_label, lv_color_hex(colors.text),
                                LV_PART_MAIN);
    lv_obj_set_pos(s_ui.time_label, metrics::kSystemPadding, 100);

    s_ui.date_label = lv_label_create(s_ui.lock_overlay);
    lv_obj_set_style_text_font(s_ui.date_label, fonts::LargeBold(),
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.date_label, lv_color_hex(colors.text),
                                LV_PART_MAIN);
    lv_obj_set_pos(s_ui.date_label, metrics::kSystemPadding, 166);

    lv_obj_t* rule = lv_obj_create(s_ui.lock_overlay);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 58, 4);
    lv_obj_set_pos(rule, metrics::kSystemPadding, 240);
    lv_obj_set_style_bg_color(rule, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* expression_host = lv_obj_create(s_ui.lock_overlay);
    lv_obj_remove_style_all(expression_host);
    lv_obj_set_size(expression_host, 600, 400);
    lv_obj_set_pos(expression_host, 60, 218);
    lv_obj_remove_flag(expression_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(expression_host, LV_OBJ_FLAG_CLICKABLE);
    s_ui.expression = new ExpressionPlayer(expression_host);
    s_ui.expression->Sleep();

    lv_obj_t* track = lv_obj_create(s_ui.lock_overlay);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, kSliderWidth, kSliderHeight);
    lv_obj_set_pos(track, kSliderX, kSliderY);
    lv_obj_set_style_radius(track, metrics::kRadiusPanel, LV_PART_MAIN);
    lv_obj_set_style_bg_color(track, lv_color_hex(colors.surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(track, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(track, lv_color_hex(colors.border),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.slider_fill = lv_obj_create(track);
    lv_obj_remove_style_all(s_ui.slider_fill);
    lv_obj_set_height(s_ui.slider_fill, kSliderHeight);
    lv_obj_set_pos(s_ui.slider_fill, 0, 0);
    lv_obj_set_style_radius(s_ui.slider_fill, metrics::kRadiusPanel,
                            LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        s_ui.slider_fill, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.slider_fill, LV_OPA_20, LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.slider_fill, LV_OBJ_FLAG_CLICKABLE);

    s_ui.slider_label = lv_label_create(track);
    lv_label_set_text(s_ui.slider_label, "向右滑动解锁");
    lv_obj_set_style_text_font(s_ui.slider_label, fonts::MediumBold(),
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.slider_label, lv_color_hex(colors.muted),
                                LV_PART_MAIN);
    lv_obj_center(s_ui.slider_label);
    lv_obj_remove_flag(s_ui.slider_label, LV_OBJ_FLAG_CLICKABLE);

    s_ui.slider_thumb = lv_obj_create(track);
    lv_obj_remove_style_all(s_ui.slider_thumb);
    lv_obj_set_size(s_ui.slider_thumb, kSliderThumbSize, kSliderThumbSize);
    lv_obj_set_pos(s_ui.slider_thumb, kSliderInset, kSliderInset);
    lv_obj_set_style_radius(s_ui.slider_thumb, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        s_ui.slider_thumb, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.slider_thumb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.slider_thumb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.slider_thumb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon = lv_label_create(s_ui.slider_thumb);
    lv_label_set_text(icon,
                      FONT_AWESOME_ANGLE_RIGHT FONT_AWESOME_ANGLE_RIGHT);
    lv_obj_set_style_text_font(icon, fonts::IconLarge(), LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(colors.accent_ink),
                                LV_PART_MAIN);
    lv_obj_center(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(s_ui.slider_thumb, OnSliderPressed, LV_EVENT_PRESSED,
                        nullptr);
    lv_obj_add_event_cb(s_ui.slider_thumb, OnSliderPressing, LV_EVENT_PRESSING,
                        nullptr);
    lv_obj_add_event_cb(s_ui.slider_thumb, OnSliderReleased, LV_EVENT_RELEASED,
                        nullptr);
    lv_obj_add_event_cb(s_ui.slider_thumb, OnSliderReleased,
                        LV_EVENT_PRESS_LOST, nullptr);

    ResetSlider();
    UpdateClock(nullptr);
    s_ui.clock_timer = lv_timer_create(UpdateClock, 1000, nullptr);
    lv_obj_move_foreground(s_ui.lock_overlay);
}

void DeleteLockUi() {
    DeleteTimer(s_ui.clock_timer);
    delete s_ui.expression;
    s_ui.expression = nullptr;
    if (s_ui.lock_overlay != nullptr &&
        lv_obj_is_valid(s_ui.lock_overlay)) {
        lv_obj_delete(s_ui.lock_overlay);
    }
    s_ui.lock_overlay = nullptr;
    s_ui.black_overlay = nullptr;
    s_ui.time_label = nullptr;
    s_ui.date_label = nullptr;
    s_ui.slider_fill = nullptr;
    s_ui.slider_label = nullptr;
    s_ui.slider_thumb = nullptr;
}

void EnterScreenOff() {
    DeleteTimer(s_ui.off_timer);
    if (s_ui.peripherals_pending) {
        s_ui.sleep_after_wake = true;
        s_ui.immediate_sleep_after_wake = true;
        return;
    }
    s_ui.peripherals_failed = false;
    if (!s_ui.active || s_ui.black || s_ui.lock_overlay == nullptr ||
        !lv_obj_is_valid(s_ui.lock_overlay)) {
        return;
    }

    s_ui.dragging = false;
    if (s_ui.expression != nullptr) {
        s_ui.expression->SetRenderingPaused(true);
    }
    s_ui.black_overlay = lv_obj_create(s_ui.lock_overlay);
    lv_obj_remove_style_all(s_ui.black_overlay);
    lv_obj_set_size(s_ui.black_overlay, metrics::kDisplaySize,
                    metrics::kDisplaySize);
    lv_obj_set_pos(s_ui.black_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_ui.black_overlay, lv_color_black(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.black_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.black_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.black_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_ui.black_overlay);
    lv_refr_now(nullptr);
    s_ui.black = true;
    s_screen_off_snapshot.store(true, std::memory_order_release);
    if (s_ui.clock_timer != nullptr) {
        lv_timer_pause(s_ui.clock_timer);
    }
    // The adapter detaches the LVGL display before deleting the DPI producer.
    // Keep all invalidation suppressed until the new panel is rebound.
    SuspendDisplayInvalidation();

    Application::GetInstance().SetLowPowerStandby(true);
    bool panel_suspended = true;
    Display* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        panel_suspended = display->SetPowerSaveModeChecked(true);
    }
    if (!panel_suspended) {
        // A failed delete/recovery can leave the adapter detached. Preserve
        // the black overlay and invalidation guard until a later key retry.
        if (display != nullptr && display->IsPowerSaveActive()) {
            PerformanceManager::Get().SetStandbyPhase(
                StandbyPerformancePhase::ScreenOff);
            Board::GetInstance().SetLowPowerStandby(true);
            if (Backlight* backlight = Board::GetInstance().GetBacklight()) {
                backlight->SetBrightness(0, false);
            }
            ESP_LOGE(kTag,
                     "Panel suspend failed after detachment; keeping UI black for recovery");
            return;
        }
        Application::GetInstance().SetLowPowerStandby(false);
        if (s_ui.black_overlay != nullptr &&
            lv_obj_is_valid(s_ui.black_overlay)) {
            lv_obj_delete(s_ui.black_overlay);
        }
        s_ui.black_overlay = nullptr;
        s_ui.black = false;
        s_screen_off_snapshot.store(false, std::memory_order_release);
        if (s_ui.expression != nullptr) {
            s_ui.expression->SetRenderingPaused(false);
        }
        if (s_ui.clock_timer != nullptr) {
            lv_timer_resume(s_ui.clock_timer);
        }
        ResumeDisplayInvalidation();
        lv_obj_invalidate(s_ui.lock_overlay);
        lv_refr_now(nullptr);
        ESP_LOGW(kTag,
                 "Screen-off cancelled because panel suspend failed; "
                 "automatic retry disabled until the next user action");
        return;
    }
    PerformanceManager::Get().SetStandbyPhase(
        StandbyPerformancePhase::ScreenOff);
    Board::GetInstance().SetLowPowerStandby(true);
    if (Backlight* backlight = Board::GetInstance().GetBacklight()) {
        backlight->SetBrightness(0, false);
    }
    ESP_LOGI(kTag, "Lock screen off; low-power standby active");
}

void OnScreenOff(lv_timer_t*) {
    s_ui.off_timer = nullptr;
    EnterScreenOff();
}

void ScheduleScreenOff() {
    DeleteTimer(s_ui.off_timer);
    if (!s_ui.active || s_ui.black || s_ui.peripherals_pending) return;
    s_ui.off_timer = lv_timer_create(OnScreenOff, kScreenOffDelayMs, nullptr);
    lv_timer_set_repeat_count(s_ui.off_timer, 1);
}

void WakeLockScreen() {
    ESP_LOGI(kTag, "Wake requested (active=%d screen_off=%d)",
             s_ui.active, s_ui.black);
    if (!s_ui.active || !s_ui.black) return;

    // Hardware recovery runs on the power-key worker before this LVGL-only
    // callback. Keep the UI paused and black if that recovery did not finish.
    if (Display* display = Board::GetInstance().GetDisplay()) {
        if (display->IsPowerSaveActive()) {
            ESP_LOGE(kTag,
                     "Display recovery is still pending; retaining black standby state");
            return;
        }
        if (!display->SetPowerSaveModeChecked(false)) {
            ESP_LOGE(kTag, "Display wake failed; retaining black standby state");
            return;
        }
    }

    // Show the recovered panel before waiting for radio/audio clocks. Keep
    // the application audio gate closed until the worker confirms readiness.
    PerformanceManager::Get().SetStandbyPhase(
        StandbyPerformancePhase::Awake);
    if (Display* display = Board::GetInstance().GetDisplay()) {
        ESP_LOGI(kTag, "Display panel recovery confirmed");
    }
    if (s_ui.black_overlay != nullptr &&
        lv_obj_is_valid(s_ui.black_overlay)) {
        lv_obj_delete(s_ui.black_overlay);
    }
    s_ui.black_overlay = nullptr;
    s_ui.black = false;
    s_screen_off_snapshot.store(false, std::memory_order_release);
    if (s_ui.expression != nullptr) {
        s_ui.expression->SetRenderingPaused(false);
    }
    ResetSlider();
    UpdateClock(nullptr);
    if (s_ui.clock_timer != nullptr) {
        lv_timer_resume(s_ui.clock_timer);
    }
    // Resume the panel first, then permit drawing and force a clean full-frame
    // refresh. This keeps RGB888 line/byte phase aligned across screen-off.
    ResumeDisplayInvalidation();
    if (Backlight* backlight = Board::GetInstance().GetBacklight()) {
        backlight->RestoreBrightness();
    }
    StatusBar::Get().SetLockScreenMode(true);
    StartPeripheralWake();
    lv_obj_invalidate(s_ui.lock_overlay);
    lv_refr_now(nullptr);
    PerformanceManager::Get().SetStandbyPhase(
        StandbyPerformancePhase::Dim);
    ScheduleScreenOff();
    ESP_LOGI(kTag,
             "Lock screen visible; peripheral recovery continues on worker");
}

void UnlockToSource() {
    if (!s_ui.active || s_ui.black) return;
    DeleteTimer(s_ui.off_timer);

    PerformanceManager::Get().SetStandbyPhase(
        StandbyPerformancePhase::Awake);
    // The worker owns hardware recovery. Unlock must not repeat it or open
    // audio before the peripheral clocks have been confirmed ready.
    if (s_ui.audio_ready || (!s_ui.peripherals_pending && !s_ui.peripherals_failed)) {
        AudioOutput_SetStandby(false);
    }
    StatusBar::Get().SetLockScreenMode(false);

    DeleteLockUi();
    if (s_ui.source_screen != nullptr &&
        lv_obj_is_valid(s_ui.source_screen)) {
        SendAppLifecycle(s_ui.source_screen, AppLifecycleEvent::Resume);
        if (s_ui.source_screen == home::Renderer::Screen()) {
            home::Renderer::SetRenderingPaused(false);
        }
    }
    if (Backlight* backlight = Board::GetInstance().GetBacklight()) {
        backlight->RestoreBrightness();
    }
    s_ui.active = false;
    s_ui.black = false;
    s_active_snapshot.store(false, std::memory_order_release);
    s_screen_off_snapshot.store(false, std::memory_order_release);
    s_ui.dragging = false;
    s_ui.source_screen = nullptr;
    IdlePower::Get().SetStandbyActive(false);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
    ESP_LOGI(kTag, "Slide unlock restored the preserved screen");
}

}  // namespace

void StandbyView::Show(bool screen_off_immediately) {
    if (s_ui.peripherals_pending) {
        s_ui.sleep_after_wake = true;
        s_ui.immediate_sleep_after_wake |= screen_off_immediately;
        return;
    }
    if (s_ui.active) {
        if (screen_off_immediately) EnterScreenOff();
        return;
    }
    ResumeDisplayInvalidation();
    s_ui.source_screen = lv_screen_active();
    if (s_ui.source_screen == nullptr ||
        !lv_obj_is_valid(s_ui.source_screen)) {
        ESP_LOGW(kTag, "Standby ignored because no screen is mounted");
        s_ui.source_screen = nullptr;
        return;
    }

    Application::GetInstance().ForceReturnToIdle();
    s_ui.active = true;
    s_ui.black = false;
    s_active_snapshot.store(true, std::memory_order_release);
    s_screen_off_snapshot.store(false, std::memory_order_release);
    s_ui.dragging = false;
    s_ui.black_overlay = nullptr;
    SendAppLifecycle(s_ui.source_screen, AppLifecycleEvent::Suspend);
    if (s_ui.source_screen == home::Renderer::Screen()) {
        home::Renderer::SetRenderingPaused(true);
    }
    CreateLockUi();
    StatusBar::Get().SetLockScreenMode(true);
    IdlePower::Get().SetStandbyActive(true);
    AudioOutput_SetStandby(true);
    if (Backlight* backlight = Board::GetInstance().GetBacklight()) {
        backlight->SetBrightness(kDimBrightnessPercent, false);
    }
    if (screen_off_immediately) {
        ESP_LOGI(kTag, "Immediate screen-off requested");
        EnterScreenOff();
        return;
    }
    ScheduleScreenOff();
    ESP_LOGI(kTag,
             "Lock screen active at %u%% brightness; screen-off in %u ms",
             static_cast<unsigned>(kDimBrightnessPercent),
             static_cast<unsigned>(kScreenOffDelayMs));
}

void StandbyView::HandlePowerKey() {
    if (s_ui.peripherals_pending) return;
    if (s_ui.peripherals_failed) {
        StartPeripheralWake();
        return;
    }
    if (!s_ui.active) {
        Show();
        return;
    }
    if (s_ui.black) {
        WakeLockScreen();
    } else {
        EnterScreenOff();
    }
}

void StandbyView::CompleteAudioWake() {
    if (s_ui.black || !s_ui.peripherals_pending || s_ui.audio_ready) return;
    s_ui.audio_ready = true;
    Application::GetInstance().SetLowPowerStandby(false);
    Board::GetInstance().CompleteLowPowerWake();
    if (!s_ui.active) AudioOutput_SetStandby(false);
    ESP_LOGI(kTag, "Audio resumed; network recovery continues in background");
}

void StandbyView::CompletePeripheralWake(bool ready) {
    if (s_ui.black || !s_ui.peripherals_pending) return;
    if (ready) CompleteAudioWake();
    s_ui.peripherals_pending = false;
    s_ui.peripherals_failed = !ready;
    if (!ready) {
        ESP_LOGW(kTag, "Peripheral wake incomplete; UI remains available, audio_ready=%d", s_ui.audio_ready);
    } else {
        ESP_LOGI(kTag, "Background peripherals recovered");
    }
    if (s_ui.sleep_after_wake) {
        const bool immediate = s_ui.immediate_sleep_after_wake;
        s_ui.sleep_after_wake = s_ui.immediate_sleep_after_wake = false;
        StandbyView::Show(immediate);
    } else if (s_ui.active) {
        ScheduleScreenOff();
    }
}

void StandbyView::WakeScreen() {
    if (!s_ui.active || !s_ui.black) return;
    WakeLockScreen();
}

bool StandbyView::IsActive() {
    return s_active_snapshot.load(std::memory_order_acquire);
}

bool StandbyView::IsScreenOff() {
    return s_screen_off_snapshot.load(std::memory_order_acquire);
}

}  // namespace agent_ui
