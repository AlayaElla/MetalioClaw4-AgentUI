#include "power_key.h"

#include <atomic>

#include <esp_lv_adapter.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "IOExpander.hpp"
#include "lvgl.h"
#include "apps/power/power_view.h"
#include "apps/standby/standby_view.h"
#include "board.h"
#include "display.h"

namespace agent_ui {
namespace {
constexpr char kTag[] = "AgentPowerKey";
constexpr uint32_t kLongPressMs = 1500;
constexpr UBaseType_t kQueueDepth = 4;
constexpr uint32_t kWorkerStackBytes = 8 * 1024;
enum class KeyEvent : uint8_t { ShortPress, LongPress, RestorePeripherals, CheckStandby };
struct KeyRequest {
    KeyEvent event;
    bool wake_only;
};

bool s_initialized = false;
bool s_worker_started = false;
QueueHandle_t s_key_queue = nullptr;
std::atomic<bool> s_ui_dispatch_pending{false};
std::atomic<bool> s_wake_in_progress{false};
std::atomic<KeyEvent> s_pending_event{KeyEvent::ShortPress};
std::atomic<bool> s_pending_wake_only{false};
std::atomic<bool> s_peripheral_resume_pending{false};
std::atomic<bool> s_peripheral_resume_ready{false};
std::atomic<bool> s_peripheral_audio_ready{false};

void OnAudioReadyUi(void*) {
    if (s_peripheral_audio_ready.load()) StandbyView::CompleteAudioWake();
}

void OnPeripheralsReadyUi(void*) {
    // LVGL timers may run in either order. Always deliver audio readiness
    // before final completion; a later audio callback becomes a no-op.
    OnAudioReadyUi(nullptr);
    s_peripheral_resume_pending.store(false, std::memory_order_release);
    StandbyView::CompletePeripheralWake(s_peripheral_resume_ready.load());
}

void PostWakeCallback(void (*callback)(void*)) {
    // A full LVGL callback queue must not strand the visible recovery screen.
    // Retry posting without repeating hardware transitions or holding the lock.
    while (true) {
        if (esp_lv_adapter_lock(100) == ESP_OK) {
            const bool posted = lv_async_call(callback, nullptr) == LV_RESULT_OK;
            esp_lv_adapter_unlock();
            if (posted) return;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void RestorePeripherals() {
    s_peripheral_resume_ready.store(Board::GetInstance().ResumeLowPowerStandby([] {
        s_peripheral_audio_ready.store(true);
        PostWakeCallback(OnAudioReadyUi);
    }));
    PostWakeCallback(OnPeripheralsReadyUi);
}

void OnPowerKeyUi(void*) {
    const KeyEvent event = s_pending_event.load(std::memory_order_acquire);
    ESP_LOGI(kTag, "Power key dispatched to LVGL (long=%d standby=%d screen_off=%d)",
             event == KeyEvent::LongPress, StandbyView::IsActive(),
             StandbyView::IsScreenOff());

    // A timed wake can restore the panel just before its own UI callback is
    // queued. Treat a key press in that interval as the same wake request.
    if (s_pending_wake_only.load(std::memory_order_acquire) ||
        (StandbyView::IsActive() && StandbyView::IsScreenOff())) {
        StandbyView::WakeScreen();
    } else if (event == KeyEvent::LongPress && !StandbyView::IsActive()) {
        PowerView::ShowDialog();
    } else {
        StandbyView::HandlePowerKey();
    }
    s_wake_in_progress.store(false, std::memory_order_release);
    s_ui_dispatch_pending.store(false, std::memory_order_release);
}

void ProcessKeyEvent(KeyRequest request) {
    bool expected = false;
    if (!s_ui_dispatch_pending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        ESP_LOGD(kTag, "Coalesced power-key event while UI dispatch is pending");
        return;
    }

    Display* display = Board::GetInstance().GetDisplay();
    bool wake_only = request.wake_only;
    struct WakePreparation {
        bool cancel = false;
        ~WakePreparation() { if (cancel) Board::GetInstance().CancelLowPowerWake(); }
    } preparation;
    if (display != nullptr && display->IsPowerSaveActive()) {
        wake_only = true;
        preparation.cancel = Board::GetInstance().PrepareLowPowerWake();
    }
    if (wake_only) {
        s_wake_in_progress.store(true, std::memory_order_release);
        if (display != nullptr && display->IsPowerSaveActive() &&
            !display->PrepareWakeFromPowerSave()) {
            ESP_LOGW(kTag, "Display recovery failed; leaving standby black for retry");
            s_wake_in_progress.store(false, std::memory_order_release);
            s_ui_dispatch_pending.store(false, std::memory_order_release);
            return;
        }
    }

    s_pending_event.store(request.event, std::memory_order_release);
    s_pending_wake_only.store(wake_only, std::memory_order_release);
    // lv_async_call updates LVGL timer state, so serialize it with the adapter
    // lock. This worker has an 8 KB stack for the bounded panel recovery path.
    if (esp_lv_adapter_lock(portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to lock LVGL adapter for power-key dispatch");
        s_wake_in_progress.store(false, std::memory_order_release);
        s_ui_dispatch_pending.store(false, std::memory_order_release);
        return;
    }
    if (lv_async_call(OnPowerKeyUi, nullptr) != LV_RESULT_OK) {
        ESP_LOGE(kTag, "LVGL rejected power-key callback");
        esp_lv_adapter_unlock();
        s_wake_in_progress.store(false, std::memory_order_release);
        s_ui_dispatch_pending.store(false, std::memory_order_release);
        return;
    }
    esp_lv_adapter_unlock();
    preparation.cancel = false;
}

void PowerKeyWorker(void*) {
    KeyRequest request{};
    while (true) {
        const TickType_t wait = StandbyView::IsScreenOff()
                                   ? pdMS_TO_TICKS(1000) : portMAX_DELAY;
        if (xQueueReceive(s_key_queue, &request, wait) == pdTRUE) {
            if (request.event == KeyEvent::RestorePeripherals) RestorePeripherals();
            else if (request.event == KeyEvent::CheckStandby) Board::GetInstance().TickLowPowerStandby();
            else ProcessKeyEvent(request);
        } else {
            Board::GetInstance().TickLowPowerStandby();
        }
    }
}

void QueueKeyEvent(KeyEvent event) {
    if (s_key_queue == nullptr) return;
    // Additional presses during recovery belong to the same wake, not a
    // delayed request to immediately black out the newly restored screen.
    if (s_peripheral_resume_pending.load(std::memory_order_acquire)) return;
    if (s_ui_dispatch_pending.load(std::memory_order_acquire)) {
        ESP_LOGD(kTag, "Coalesced power-key event while UI dispatch is pending");
        return;
    }
    Display* display = Board::GetInstance().GetDisplay();
    const bool wake_only =
        s_wake_in_progress.load(std::memory_order_acquire) ||
        (StandbyView::IsActive() && StandbyView::IsScreenOff()) ||
        (display != nullptr && display->IsPowerSaveActive());
    const KeyRequest request{event, wake_only};
    if (xQueueSend(s_key_queue, &request, 0) != pdTRUE) {
        ESP_LOGW(kTag, "Power-key event queue is full; ignoring event");
    }
}

void OnShort() { QueueKeyEvent(KeyEvent::ShortPress); }
void OnLong() { QueueKeyEvent(KeyEvent::LongPress); }
}

bool PowerKey::ResumeStandbyPeripherals() {
    if (s_key_queue == nullptr) return false;
    bool expected = false;
    if (!s_peripheral_resume_pending.compare_exchange_strong(expected, true)) return true;
    s_peripheral_audio_ready.store(false);
    const KeyRequest request{KeyEvent::RestorePeripherals, true};
    if (xQueueSend(s_key_queue, &request, 0) == pdTRUE) return true;
    s_peripheral_resume_pending.store(false);
    return false;
}

void PowerKey::NotifyStandbyStarted() {
    if (s_key_queue == nullptr) return;
    const KeyRequest request{KeyEvent::CheckStandby, false};
    // If full, an existing request already wakes the worker; its next wait
    // observes the black-screen snapshot and checks the deadline in one second.
    (void)xQueueSend(s_key_queue, &request, 0);
}

void PowerKey::Initialize() {
    if (s_initialized) return;
    if (!s_worker_started) {
        s_key_queue = xQueueCreate(kQueueDepth, sizeof(KeyRequest));
        if (s_key_queue == nullptr) {
            ESP_LOGE(kTag, "Power-key queue allocation failed");
            return;
        }
        if (xTaskCreate(PowerKeyWorker, "power_key_wake", kWorkerStackBytes,
                        nullptr, 5, nullptr) != pdPASS) {
            ESP_LOGE(kTag, "Power-key worker creation failed");
            vQueueDelete(s_key_queue);
            s_key_queue = nullptr;
            return;
        }
        s_worker_started = true;
    }
    auto& io = IOExpander::getInstance();
    const esp_err_t result = io.onShortOrLongPress(
        IOExpander::Pin::PWR_KEY, kLongPressMs, OnShort, OnLong);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Power key registration failed: %d", result);
        return;
    }
    s_initialized = true;
}

}  // namespace agent_ui
