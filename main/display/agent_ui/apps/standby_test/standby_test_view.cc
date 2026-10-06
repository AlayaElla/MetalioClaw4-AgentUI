#include "standby_test_view.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <functional>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_private/esp_clk.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "apps/standby/standby_view.h"
#include "board.h"
#include "core/app_shell.h"
#include "core/fonts.h"
#include "core/agent_ui_types.h"
#include "core/performance_manager.h"
#include "core/theme.h"
#include "core/ui_utils.h"
#include "display.h"
#include "ui_dispatcher.h"
#include "standby_test_controller.h"

namespace agent_ui {
namespace {

constexpr char kTag[] = "StandbyTest";
constexpr uint32_t kBlackSettleMs = 2000;
constexpr uint32_t kBlackDurationMs = 15000;
constexpr uint32_t kLongBlackDurationMs = 60000;
constexpr uint32_t kWakePollMs = 25;
// A complete hardware sleep/recovery used 1324 bytes of the former 8 KiB
// stack. Keep 4 KiB reserved, leaving headroom without starving SDIO's heap.
constexpr uint32_t kWorkerStackBytes = 4096;

struct UiState {
    lv_obj_t* root = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* current = nullptr;
    lv_obj_t* applied = nullptr;
    lv_obj_t* battery = nullptr;
    lv_obj_t* power = nullptr;
    lv_timer_t* refresh_timer = nullptr;
};

UiState s_ui;
std::atomic<bool> s_test_running{false};
std::atomic<bool> s_cancel_requested{false};
std::atomic<uint32_t> s_generation{0};
std::atomic<uint32_t> s_requested_generation{0};
std::atomic<uint32_t> s_requested_duration_ms{kBlackDurationMs};
TaskHandle_t s_wake_task = nullptr;
DRAM_ATTR StaticTask_t s_wake_task_storage;
alignas(portBYTE_ALIGNMENT) DRAM_ATTR StackType_t
    s_wake_task_stack[kWorkerStackBytes / sizeof(StackType_t)];

uint32_t NowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

Display* GetDisplay() { return Board::GetInstance().GetDisplay(); }

void SetStatus(const char* text, uint32_t color = 0x5F6B7A) {
    if (s_ui.status == nullptr || !lv_obj_is_valid(s_ui.status)) return;
    lv_label_set_text(s_ui.status, text != nullptr ? text : "");
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(color), LV_PART_MAIN);
}

void RefreshReadings(lv_timer_t*) {
    if (s_ui.root == nullptr || !lv_obj_is_valid(s_ui.root)) return;
    char text[96];
    std::snprintf(text, sizeof(text), "当前实测：%u MHz",
                  static_cast<unsigned>(esp_clk_cpu_freq() / 1000000U));
    if (s_ui.current != nullptr && lv_obj_is_valid(s_ui.current)) {
        lv_label_set_text(s_ui.current, text);
    }
    PerformanceManager& performance = PerformanceManager::Get();
    std::snprintf(text, sizeof(text), "黑屏目标：%d MHz    当前应用上限：%d MHz",
                  performance.policy().screen_off_mhz,
                  performance.current_max_mhz());
    if (s_ui.applied != nullptr && lv_obj_is_valid(s_ui.applied)) {
        lv_label_set_text(s_ui.applied, text);
    }
    if (s_ui.battery != nullptr && lv_obj_is_valid(s_ui.battery)) {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        char battery_text[64] = "电池：暂不可用";
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            std::snprintf(battery_text, sizeof(battery_text),
                          "电量：%d%%    充电：%s", level,
                          charging ? "是" : "否");
        }
        lv_label_set_text(s_ui.battery, battery_text);
    }
    if (s_ui.power != nullptr && lv_obj_is_valid(s_ui.power)) {
        BatteryPowerReading reading;
        if (Board::GetInstance().ReadBatteryPower(reading)) {
            std::snprintf(text, sizeof(text), "电池：%d mV   电流：%+d mA（正充 / 负放）",
                          reading.voltage_mv, reading.current_ma);
            lv_label_set_text(s_ui.power, text);
        } else {
            lv_label_set_text(s_ui.power, "电池电压 / 电流：暂不可用");
        }
    }
}

bool IsCurrentGeneration(uint32_t generation) {
    return s_generation.load() == generation;
}

bool PostUi(uint32_t generation, std::function<void()> callback) {
    while (!s_cancel_requested.load() && IsCurrentGeneration(generation)) {
        if (UiDispatcher::Post(callback)) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

void ReportInterrupted(uint32_t generation, uint32_t elapsed_ms,
                       uint32_t actual_mhz = 0, int applied_mhz = -1) {
    PostUi(generation, [generation, elapsed_ms, actual_mhz, applied_mhz]() {
        if (!IsCurrentGeneration(generation)) return;
        if (s_ui.root == nullptr || !lv_obj_is_valid(s_ui.root)) return;
        char text[256];
        if (actual_mhz != 0) {
            std::snprintf(text, sizeof(text),
                          "测试提前结束；黑屏实测 %lu MHz（应用上限 %d MHz）\n"
                          "黑屏 %lu ms，已保留本次测量",
                          static_cast<unsigned long>(actual_mhz), applied_mhz,
                          static_cast<unsigned long>(elapsed_ms));
        } else {
            std::snprintf(text, sizeof(text),
                          "侧键已唤醒，测试取消（黑屏 %lu ms）",
                          static_cast<unsigned long>(elapsed_ms));
        }
        SetStatus(text, 0xD97706);
    });
}

void LogPowerManagementState(const char* phase) {
    esp_pm_config_t config{};
    const esp_err_t err = esp_pm_get_configuration(&config);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "PM snapshot: phase=%s max=%d min=%d light_sleep=%d",
                 phase, config.max_freq_mhz, config.min_freq_mhz,
                 config.light_sleep_enable);
    } else {
        ESP_LOGW(kTag, "PM snapshot unavailable: %s", esp_err_to_name(err));
    }
    // A momentary snapshot, not proof of the time spent asleep. CPU locks
    // include the task producing this report; USB may also inhibit sleep.
    esp_pm_dump_locks(stdout);
}

void RunTimedWakeTest(uint32_t generation, uint32_t black_duration_ms) {
    const uint32_t started_ms = NowMs();
    standby_test::Controller test;
    Display* display = GetDisplay();
    bool saw_standby_black = false;
    bool left_standby = false;
    auto fully_black = [&display]() {
        return display != nullptr && display->IsPowerSaveActive() &&
               !display->IsPanelPresent();
    };
    while (!s_cancel_requested.load() && IsCurrentGeneration(generation) &&
           !fully_black() && NowMs() - started_ms < 25000U) {
        if (StandbyView::IsScreenOff()) saw_standby_black = true;
        if (!StandbyView::IsActive() ||
            (saw_standby_black && !StandbyView::IsScreenOff())) {
            left_standby = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(kWakePollMs));
        display = GetDisplay();
    }

    if (s_cancel_requested.load() || !IsCurrentGeneration(generation) ||
        !fully_black()) {
        const uint32_t waited_ms = NowMs() - started_ms;
        const bool timed_out = waited_ms >= 25000U && !left_standby;
        const bool panel_still_present =
            display != nullptr && display->IsPanelPresent();
        const bool power_save =
            display != nullptr && display->IsPowerSaveActive();
        PostUi(generation, [generation, timed_out, panel_still_present,
                            power_save]() {
            if (!IsCurrentGeneration(generation) || s_ui.root == nullptr ||
                !lv_obj_is_valid(s_ui.root)) return;
            const char* status = "待机阶段被手动唤醒，测试已取消";
            uint32_t color = 0xD97706;
            if (timed_out && panel_still_present && power_save) {
                status = "显示驱动仍在运行；未进行黑屏采样";
                color = 0xD14343;
            } else if (timed_out) {
                status = "等待真正黑屏超时；请检查待机状态";
                color = 0xD14343;
            }
            SetStatus(status, color);
        });
        return;
    }

    const uint32_t black_started_ms = NowMs();
    const auto sleep_before = Board::GetInstance().GetStandbySleepStats();
    test.Start(black_started_ms);
    test.ScreenTurnedOff();
    // Print before settling so diagnostic output stays outside the samples.
    LogPowerManagementState("black_before_settle");
    vTaskDelay(pdMS_TO_TICKS(kBlackSettleMs));
    if (s_cancel_requested.load() || !IsCurrentGeneration(generation) ||
        !fully_black()) {
        test.Cancel();
        if (display != nullptr && display->IsPowerSaveActive() &&
            display->IsPanelPresent()) {
            PostUi(generation, [generation]() {
                if (IsCurrentGeneration(generation))
                    SetStatus("显示驱动仍活动；没有采集黑屏频率", 0xD14343);
            });
        } else {
            ReportInterrupted(generation, NowMs() - black_started_ms);
        }
        return;
    }

    // Sample only after the panel has reported power-save and settled. This is
    // deliberately outside LVGL because the display adapter may pause LVGL.
    test.BeginMeasurement();
    const uint32_t actual_mhz = esp_clk_cpu_freq() / 1000000U;
    const int applied_mhz = PerformanceManager::Get().current_max_mhz();
    test.RecordBlackScreenFrequency(actual_mhz);
    standby_test::PowerSamples power_samples;
    standby_test::SamplingWindow sampling(black_duration_ms);
    uint32_t sample_attempts = 0;
    ESP_LOGI(kTag, "Black-screen sample: actual=%lu MHz applied=%d MHz",
             static_cast<unsigned long>(actual_mhz), applied_mhz);
    while (!s_cancel_requested.load() && IsCurrentGeneration(generation) &&
           fully_black() &&
           NowMs() - black_started_ms < black_duration_ms) {
        const uint32_t elapsed_ms = NowMs() - black_started_ms;
        if (sampling.Due(elapsed_ms)) {
            ++sample_attempts;
            BatteryPowerReading reading;
            if (Board::GetInstance().ReadBatteryPower(reading) && fully_black()) {
                power_samples.Add(reading.current_ma, reading.external_power);
                ESP_LOGI(kTag, "Black-screen battery: voltage=%d mV current=%+d mA external=%d",
                         reading.voltage_mv, reading.current_ma, reading.external_power);
            } else {
                ESP_LOGW(kTag, "Battery sample failed: attempt=%lu elapsed=%lu ms",
                         static_cast<unsigned long>(sample_attempts),
                         static_cast<unsigned long>(elapsed_ms));
            }
            sampling.SampleAttempted(NowMs() - black_started_ms);
        }
        const uint32_t wait_ms = sampling.WaitMs(NowMs() - black_started_ms);
        if (wait_ms != 0) vTaskDelay(pdMS_TO_TICKS(wait_ms) + 1);
    }

    if (s_cancel_requested.load() || !IsCurrentGeneration(generation) ||
        !fully_black()) {
        test.Cancel();
        if (display->IsPowerSaveActive() && display->IsPanelPresent()) {
            PostUi(generation, [generation]() {
                if (IsCurrentGeneration(generation))
                    SetStatus("黑屏测试中止：显示驱动仍在运行", 0xD14343);
            });
        } else {
            ReportInterrupted(generation, NowMs() - black_started_ms, actual_mhz,
                              applied_mhz);
        }
        return;
    }

    const auto sleep_after = Board::GetInstance().GetStandbySleepStats();
    const uint32_t sleep_ms = static_cast<uint32_t>(
        (sleep_after.slept_us - sleep_before.slept_us) / 1000);
    const uint32_t sleep_entries = sleep_after.entries - sleep_before.entries;
    ESP_LOGI(kTag, "Sleep sample: successful=%lu duration=%lu ms rejected=%lu",
             static_cast<unsigned long>(sleep_entries),
             static_cast<unsigned long>(sleep_ms),
             static_cast<unsigned long>(sleep_after.rejected - sleep_before.rejected));

    // The firmware standby hook resumes the panel/worker without touching
    // LVGL, then the wake-only callback restores the retained lock UI.
    const bool wake_prepared = Board::GetInstance().PrepareLowPowerWake();
    if (!display->PrepareWakeFromPowerSave()) {
        if (wake_prepared) Board::GetInstance().CancelLowPowerWake();
        PostUi(generation, [generation]() {
            if (IsCurrentGeneration(generation))
                SetStatus("自动唤醒失败，请按侧键唤醒后重试", 0xD14343);
        });
        return;
    }

    ESP_LOGI(kTag, "Black-screen average: current=%+d mA samples=%lu battery_only=%d attempts=%lu",
             power_samples.average_ma(), static_cast<unsigned long>(power_samples.count()),
             power_samples.battery_only(), static_cast<unsigned long>(sample_attempts));
    LogPowerManagementState("panel_recovered");
    PostUi(generation, [generation, test, applied_mhz, power_samples,
                        sleep_ms, sleep_entries, sample_attempts]() mutable {
        if (!IsCurrentGeneration(generation)) return;
        StandbyView::WakeScreen();
        if (s_ui.root == nullptr || !lv_obj_is_valid(s_ui.root)) return;
        Display* resumed_display = GetDisplay();
        const bool recovered_lock = StandbyView::IsActive() &&
                                    !StandbyView::IsScreenOff() &&
                                    resumed_display != nullptr &&
                                    !resumed_display->IsPowerSaveActive() &&
                                    resumed_display->IsPanelPresent();
        test.Woke(NowMs(), recovered_lock);
        const auto result = test.result();
        char text[640];
        char power_text[160];
        if (power_samples.count() == 0) {
            std::snprintf(power_text, sizeof(power_text), "黑屏电流未读到，请重试");
        } else {
            std::snprintf(power_text, sizeof(power_text), "黑屏平均电流：%+d mA（有效%lu/%lu次）%s",
                          power_samples.average_ma(),
                          static_cast<unsigned long>(power_samples.count()),
                          static_cast<unsigned long>(sample_attempts),
                          power_samples.battery_only() ? "" : "；连接电源，仅供参考");
        }
        std::snprintf(text, sizeof(text),
                      "黑屏实测 %lu MHz（应用上限 %d MHz）\n"
                      "黑屏 %lu ms；%s\n%s\n"
                      "轻睡眠 %lu ms（%lu次）\n%s",
                      static_cast<unsigned long>(result.black_screen_mhz),
                      applied_mhz,
                      static_cast<unsigned long>(result.elapsed_ms),
                      result.woke_to_lock_screen ? "已恢复待机锁屏"
                                                 : "未能恢复待机锁屏", power_text,
                      static_cast<unsigned long>(sleep_ms),
                      static_cast<unsigned long>(sleep_entries),
                      "本次保留联网；电流不代表10分钟后的长待机");
        const int unmet = PerformanceManager::Get().policy().screen_off_mhz;
        SetStatus(text, result.woke_to_lock_screen &&
                            result.black_screen_mhz <= static_cast<uint32_t>(unmet)
                        ? 0xD97706
                        : 0xD14343);
        if (s_ui.applied != nullptr && lv_obj_is_valid(s_ui.applied)) {
            char applied_text[64];
            std::snprintf(applied_text, sizeof(applied_text),
                          "黑屏应用上限：%d MHz", applied_mhz);
            lv_label_set_text(s_ui.applied, applied_text);
        }
    });
}

void TimedWakeTask(void*) {
    // One task owns these static resources for the firmware's lifetime.
    // Never delete/recreate its TCB while the idle task might still clean it.
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t generation = s_requested_generation.load();
        RunTimedWakeTest(generation, s_requested_duration_ms.load());
        ESP_LOGI(kTag, "Test worker idle: generation=%lu stack_free=%u bytes",
                 static_cast<unsigned long>(generation),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        // Release only after the run has returned and its locals are gone.
        s_test_running.store(false);
    }
}

void OnManualStandby(lv_event_t*) {
    if (s_test_running.load()) return;
    SetStatus("立即黑屏；前10分钟保持联网，之后断网省电", 0x2563EB);
    StandbyView::Show(true);
}

void OnTimedTest(lv_event_t* event) {
    bool expected = false;
    if (!s_test_running.compare_exchange_strong(expected, true)) return;
    s_cancel_requested.store(false);
    const uint32_t generation = s_generation.fetch_add(1) + 1;
    if (s_wake_task == nullptr) {
        s_wake_task = xTaskCreateStatic(
            TimedWakeTask, "standby_test", sizeof(s_wake_task_stack), nullptr,
            4, s_wake_task_stack, &s_wake_task_storage);
    }
    if (s_wake_task == nullptr) {
        s_test_running.store(false);
        ESP_LOGE(kTag, "Static test worker unavailable");
        SetStatus("测试任务启动失败", 0xD14343);
        return;
    }
    ESP_LOGI(kTag, "Starting static test worker: generation=%lu internal_free=%u largest=%u",
             static_cast<unsigned long>(generation),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    s_requested_generation.store(generation);
    const auto requested = event != nullptr
                               ? reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) : 0;
    const uint32_t duration_ms = requested == kLongBlackDurationMs
                                     ? kLongBlackDurationMs : kBlackDurationMs;
    s_requested_duration_ms.store(duration_ms);
    SetStatus(duration_ms == kLongBlackDurationMs
                  ? "立即黑屏长测，60 秒后开始恢复" : "立即黑屏短测，15 秒后开始恢复",
              0x2563EB);
    StandbyView::Show(true);
    // Release only after the immediate transition has stopped the display.
    xTaskNotifyGive(s_wake_task);
}

lv_obj_t* CreateButton(lv_obj_t* parent, const char* label,
                       lv_event_cb_t callback, int x, int y, int width,
                       uint32_t duration_ms = kBlackDurationMs) {
    lv_obj_t* button = lv_button_create(parent);
    StyleButton(button, false);
    lv_obj_set_size(button, width, 58);
    lv_obj_set_pos(button, x, y);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(duration_ms)));
    lv_obj_t* text = lv_label_create(button);
    lv_label_set_text(text, label);
    lv_obj_set_style_text_font(text, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_center(text);
    return button;
}

void OnDeleted(lv_event_t* event) {
    if (lv_event_get_target_obj(event) != s_ui.root) return;
    s_cancel_requested.store(true);
    s_generation.fetch_add(1);
    if (s_ui.refresh_timer != nullptr) lv_timer_delete(s_ui.refresh_timer);
    s_ui = {};
}

void LifecycleCallback(AppLifecycleEvent event) {
    if (event == AppLifecycleEvent::Suspend ||
        event == AppLifecycleEvent::Unload) {
        // A physical power-key wake also invalidates the scheduled wake. The
        // worker checks the display's actual power-save state before toggling.
        if (event == AppLifecycleEvent::Unload) s_cancel_requested.store(true);
    }
}

}  // namespace

lv_obj_t* StandbyTestView::Create() {
    auto shell = CreateAppShell("待机测试", "电流、CPU 与黑屏恢复验证");
    s_ui = {};
    s_ui.root = shell.root;
    s_generation.fetch_add(1);
    lv_obj_add_event_cb(shell.root, OnDeleted, LV_EVENT_DELETE, nullptr);
    AttachAppLifecycle(shell.root, LifecycleCallback);

    const auto& colors = Theme::Get().colors();
    lv_obj_t* title = lv_label_create(shell.content);
    lv_label_set_text(title, "待机功耗测试");
    lv_obj_set_style_text_font(title, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(title, 28, 26);

    lv_obj_t* target = lv_label_create(shell.content);
    char line[96];
    std::snprintf(line, sizeof(line), "%d小时目标：≤%dmA（理论上限%.1fmA）",
                  standby_test::kTargetStandbyHours,
                  standby_test::kUsableCapacityMah / standby_test::kTargetStandbyHours,
                  static_cast<double>(standby_test::kBatteryCapacityMah) /
                      standby_test::kTargetStandbyHours);
    lv_label_set_text(target, line);
    lv_obj_set_style_text_font(target, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(target, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(target, 28, 86);

    s_ui.applied = lv_label_create(shell.content);
    std::snprintf(line, sizeof(line), "当前应用上限：%d MHz",
                  PerformanceManager::Get().current_max_mhz());
    lv_label_set_text(s_ui.applied, line);
    lv_obj_set_style_text_font(s_ui.applied, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.applied, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(s_ui.applied, 28, 130);

    s_ui.current = lv_label_create(shell.content);
    lv_obj_set_style_text_font(s_ui.current, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.current, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(s_ui.current, 28, 174);

    s_ui.battery = lv_label_create(shell.content);
    lv_label_set_text(s_ui.battery, "电池：读取中");
    lv_obj_set_style_text_font(s_ui.battery, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.battery, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(s_ui.battery, 28, 218);

    s_ui.power = lv_label_create(shell.content);
    lv_obj_set_style_text_font(s_ui.power, fonts::Small(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.power, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(s_ui.power, 28, 262);

    s_ui.status = lv_label_create(shell.content);
    lv_label_set_text(s_ui.status, "黑屏前10分钟保留网络和蓝牙，之后断网省电\n15/60秒测试测量保留联网时的电流");
    lv_obj_set_width(s_ui.status, 660);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.status, fonts::Small(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(colors.muted), LV_PART_MAIN);
    lv_obj_set_pos(s_ui.status, 28, 308);

    CreateButton(shell.content, "手动待机", OnManualStandby, 28, 472, 194);
    CreateButton(shell.content, "15秒自动唤醒", OnTimedTest, 236, 472, 214);
    CreateButton(shell.content, "60秒自动唤醒", OnTimedTest, 464, 472, 228,
                  kLongBlackDurationMs);

    s_ui.refresh_timer = lv_timer_create(RefreshReadings, 1000, nullptr);
    RefreshReadings(nullptr);
    ESP_LOGI(kTag, "Native standby test app opened");
    return shell.root;
}

}  // namespace agent_ui
