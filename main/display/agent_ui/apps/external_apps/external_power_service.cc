#include "external_power_service.h"
#include "external_power_model.h"

#ifdef METALIO_POWER_SERVICE_HOST_TEST
#include "external_power_host_platform.h"
#else
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_private/esp_clk.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "apps/standby/standby_view.h"
#include "board.h"
#include "core/performance_manager.h"
#include "display.h"
#include "ui_dispatcher.h"
#endif

namespace agent_ui::external_apps {
namespace {
constexpr char kTag[] = "ExternalPower";
constexpr uint32_t kWorkerStackBytes = 4096;
DRAM_ATTR StaticTask_t s_worker_storage;
PowerService* s_instance = nullptr;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }
bool Busy(metalio_app_standby_state_t state) {
    return state == METALIO_APP_STANDBY_WAITING ||
           state == METALIO_APP_STANDBY_MEASURING ||
           state == METALIO_APP_STANDBY_WAKING;
}
bool FullyBlack(Display* display) {
    return display && display->IsPowerSaveActive() && !display->IsPanelPresent();
}
}  // namespace

PowerService& PowerService::Get() {
    static PowerService instance;
    s_instance = &instance;
    return instance;
}
PowerService* PowerService::Existing() { return s_instance; }

int PowerService::Read(metalio_app_power_reading_t* reading) const {
    if (!reading) return METALIO_APP_POWER_ERROR_INVALID;
    *reading = {};
    reading->cpu_frequency_mhz = esp_clk_cpu_freq() / 1000000U;
    reading->applied_max_mhz = PerformanceManager::Get().current_max_mhz();
    reading->screen_off_max_mhz = PerformanceManager::Get().policy().screen_off_mhz;
    int level = 0;
    bool charging = false, discharging = false;
    if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        reading->battery_percent = static_cast<int16_t>(level);
        reading->battery_valid = 1;
        reading->charging = charging;
    }
    BatteryPowerReading battery;
    if (Board::GetInstance().ReadBatteryPower(battery)) {
        reading->power_valid = 1;
        reading->voltage_mv = battery.voltage_mv;
        reading->current_ma = battery.current_ma;
        reading->external_power = battery.external_power;
    }
    reading->standby_active = StandbyView::IsActive();
    reading->screen_off = StandbyView::IsScreenOff();
    return METALIO_APP_POWER_OK;
}

bool PowerService::Current(uint32_t generation) const {
    return generation_.load() == generation && !cancelled_.load();
}

void PowerService::Publish(uint32_t generation,
                           const metalio_app_standby_result_t& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Current(generation)) result_ = result;
}

int PowerService::GetResult(void* owner, metalio_app_standby_result_t* result) const {
    if (!owner || !result) return METALIO_APP_POWER_ERROR_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    *result = owner == owner_ ? result_ : metalio_app_standby_result_t{};
    return METALIO_APP_POWER_OK;
}

int PowerService::Start(void* owner, uint32_t duration_ms) {
    if (!owner || (duration_ms != 0 && duration_ms != 15000 && duration_ms != 60000))
        return METALIO_APP_POWER_ERROR_INVALID;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load() || Busy(result_.state)) return METALIO_APP_POWER_ERROR_BUSY;
        if (duration_ms != 0) {
            if (!worker_) {
                // Allocate only on the first timed request; retain one worker
                // so a returning run never races RTOS task/stack destruction.
                worker_stack_ = heap_caps_aligned_alloc(portBYTE_ALIGNMENT, kWorkerStackBytes,
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!worker_stack_) {
                    ESP_LOGW(kTag, "Timed test stack unavailable");
                    return METALIO_APP_POWER_ERROR_MEMORY;
                }
                worker_ = xTaskCreateStatic(Worker, "external_power", kWorkerStackBytes, this, 4,
                                            static_cast<StackType_t*>(worker_stack_), &s_worker_storage);
                if (!worker_) {
                    heap_caps_free(worker_stack_);
                    worker_stack_ = nullptr;
                    return METALIO_APP_POWER_ERROR_MEMORY;
                }
                ESP_LOGI(kTag, "Reusable timed test worker ready");
            }
            owner_ = owner;
            cancelled_.store(false);
            generation = generation_.fetch_add(1) + 1;
            result_ = {};
            result_.state = METALIO_APP_STANDBY_WAITING;
            result_.requested_duration_ms = duration_ms;
            running_.store(true);
        }
    }
    // Show dispatches App suspension. The retained standby overlay preserves
    // this owner's timed run while pausing only its UI callbacks.
    StandbyView::Show(true);
    if (!StandbyView::IsActive()) {
        if (duration_ms != 0) {
            metalio_app_standby_result_t failed{};
            failed.state = METALIO_APP_STANDBY_ERROR;
            failed.error = METALIO_APP_POWER_ERROR_SCREEN;
            failed.requested_duration_ms = duration_ms;
            Publish(generation, failed);
            running_.store(false);
        }
        return METALIO_APP_POWER_ERROR_SCREEN;
    }
    // The transition must finish before the worker observes or samples black.
    if (duration_ms != 0) xTaskNotifyGive(static_cast<TaskHandle_t>(worker_));
    return METALIO_APP_POWER_OK;
}

int PowerService::Cancel(void* owner) {
    if (!owner) return METALIO_APP_POWER_ERROR_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner != owner_) return METALIO_APP_POWER_OK;
    cancelled_.store(true);
    generation_.fetch_add(1);
    if (Busy(result_.state)) result_.state = METALIO_APP_STANDBY_CANCELLED;
    return METALIO_APP_POWER_OK;
}

void PowerService::SuspendOwner(void* owner) {
    if (!StandbyView::IsActive()) (void)Cancel(owner);
}

void PowerService::UnloadOwner(void* owner) {
    (void)Cancel(owner);
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner == owner_) {
        owner_ = nullptr;
        result_ = {};
    }
}

void PowerService::Worker(void* context) { static_cast<PowerService*>(context)->WorkerLoop(); }

void PowerService::WorkerLoop() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t generation = generation_.load();
        uint32_t duration_ms = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            duration_ms = result_.requested_duration_ms;
        }
        Run(generation, duration_ms);
        running_.store(false);
    }
}

void PowerService::Run(uint32_t generation, uint32_t duration_ms) {
    if (!Current(generation)) return;
    metalio_app_standby_result_t result{};
    result.state = METALIO_APP_STANDBY_WAITING;
    result.requested_duration_ms = duration_ms;
    Display* display = Board::GetInstance().GetDisplay();
    const uint32_t wait_started = NowMs();
    while (Current(generation) && StandbyView::IsActive() &&
           !FullyBlack(display) && NowMs() - wait_started < 25000) {
        vTaskDelay(pdMS_TO_TICKS(25));
        display = Board::GetInstance().GetDisplay();
    }
    if (!Current(generation)) return;
    if (!FullyBlack(display)) {
        result.state = StandbyView::IsScreenOff() ? METALIO_APP_STANDBY_ERROR
                                                  : METALIO_APP_STANDBY_CANCELLED;
        result.error = result.state == METALIO_APP_STANDBY_ERROR ? METALIO_APP_POWER_ERROR_SCREEN : 0;
        Publish(generation, result);
        return;
    }
    const uint32_t black_started = NowMs();
    const auto sleep_before = Board::GetInstance().GetStandbySleepStats();
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (!Current(generation)) return;
    if (!FullyBlack(display)) {
        result.state = METALIO_APP_STANDBY_CANCELLED;
        result.elapsed_ms = NowMs() - black_started;
        Publish(generation, result);
        return;
    }
    result.state = METALIO_APP_STANDBY_MEASURING;
    result.black_screen_mhz = esp_clk_cpu_freq() / 1000000U;
    result.applied_max_mhz = PerformanceManager::Get().current_max_mhz();
    Publish(generation, result);
    power_diagnostics::PowerSamples samples;
    power_diagnostics::SamplingWindow sampling(duration_ms);
    while (Current(generation) && FullyBlack(display) && NowMs() - black_started < duration_ms) {
        const uint32_t elapsed = NowMs() - black_started;
        if (sampling.Due(elapsed)) {
            ++result.sample_attempts;
            BatteryPowerReading battery;
            if (Board::GetInstance().ReadBatteryPower(battery) && FullyBlack(display))
                samples.Add(battery.current_ma, battery.external_power);
            sampling.SampleAttempted(NowMs() - black_started);
        }
        const uint32_t wait_ms = sampling.WaitMs(NowMs() - black_started);
        if (wait_ms != 0) vTaskDelay(pdMS_TO_TICKS(wait_ms) + 1);
    }
    if (!Current(generation)) return;
    result.elapsed_ms = NowMs() - black_started;
    result.sample_count = samples.count();
    result.average_current_ma = samples.average_ma();
    result.external_power = samples.count() != 0 && !samples.battery_only();
    const auto sleep_after = Board::GetInstance().GetStandbySleepStats();
    result.sleep_ms = static_cast<uint32_t>((sleep_after.slept_us - sleep_before.slept_us) / 1000);
    result.sleep_entries = sleep_after.entries - sleep_before.entries;
    result.sleep_rejected = sleep_after.rejected - sleep_before.rejected;
    if (!FullyBlack(display)) {
        result.state = METALIO_APP_STANDBY_CANCELLED;
        Publish(generation, result);
        return;
    }
    result.state = METALIO_APP_STANDBY_WAKING;
    Publish(generation, result);
    const bool prepared = Board::GetInstance().PrepareLowPowerWake();
    if (!Current(generation) || !display->PrepareWakeFromPowerSave()) {
        if (prepared) Board::GetInstance().CancelLowPowerWake();
        result.state = METALIO_APP_STANDBY_ERROR;
        result.error = METALIO_APP_POWER_ERROR_WAKE;
        Publish(generation, result);
        return;
    }
    // A queued completion contains only host-owned data. App unload or a new
    // generation invalidates it before it can wake a later screen.
    while (Current(generation)) {
        if (UiDispatcher::Post([this, generation, result]() mutable {
            if (!Current(generation)) return;
            StandbyView::WakeScreen();
            auto* restored = Board::GetInstance().GetDisplay();
            result.woke_to_lock_screen = StandbyView::IsActive() && !StandbyView::IsScreenOff() &&
                                         restored && !restored->IsPowerSaveActive() && restored->IsPanelPresent();
            result.state = result.woke_to_lock_screen ? METALIO_APP_STANDBY_COMPLETED : METALIO_APP_STANDBY_ERROR;
            result.error = result.woke_to_lock_screen ? 0 : METALIO_APP_POWER_ERROR_WAKE;
            Publish(generation, result);
        })) return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

}  // namespace agent_ui::external_apps
