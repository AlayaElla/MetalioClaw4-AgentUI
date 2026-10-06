#include "ui_dispatcher.h"

#include "lvgl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <esp_log.h>
#include <esp_timer.h>
#include <mutex>
#include <utility>

namespace {
constexpr const char* kTag = "UiDispatcher";
constexpr size_t kQueueLength = 32;
constexpr size_t kDefaultCallbackChargeBytes = 256;
// Counts retained callback payloads (including shared parsed Codex DOMs), not
// allocator overhead. Keep the ceiling aligned with the largest admitted RX
// DOM plus a small amount of ordinary UI work instead of allowing a burst to
// consume most PSRAM before admission fails.
constexpr size_t kMaxQueuedBytes = 2 * 1024 * 1024;
constexpr uint32_t kDrainPeriodMs = 10;
constexpr size_t kMaxTasksPerPass = 8;
constexpr int64_t kDrainBudgetUs = 2000;

struct Entry {
    std::function<void()> callback;
    size_t retained_bytes = 0;
    UiDispatcher::SnapshotKey coalesce_key =
        static_cast<UiDispatcher::SnapshotKey>(0);
};

std::array<Entry, kQueueLength> s_entries{};
size_t s_count = 0;
size_t s_queued_bytes = 0;
UiDispatcher::Diagnostics s_diagnostics{};
std::mutex s_queue_mutex;
std::mutex s_setup_mutex;
std::atomic<bool> s_initialized{false};
std::atomic<UiDispatcher::WakeCallback> s_wake_callback{nullptr};
std::atomic<uint32_t> s_rejections_since_log{0};
bool s_executor_configured = false;
lv_timer_t* s_timer = nullptr;

void RecordRejected() {
    const uint32_t count = s_rejections_since_log.fetch_add(
        1, std::memory_order_relaxed) + 1;
    // Keep overload diagnostics observable without letting a full queue turn
    // into a serial-log storm. Full counters remain available via GetDiagnostics.
    if ((count & 63U) == 0) {
        ESP_LOGW(kTag, "UI work rejected 64 times; inspect queue diagnostics");
    }
}

void WakeExecutor() {
    const auto wake = s_wake_callback.load(std::memory_order_acquire);
    if (wake != nullptr) wake();
}

void EraseAt(size_t index, Entry* removed) {
    if (index >= s_count) return;
    s_queued_bytes -= s_entries[index].retained_bytes;
    if (removed != nullptr) {
        removed->callback.swap(s_entries[index].callback);
        removed->retained_bytes = s_entries[index].retained_bytes;
        removed->coalesce_key = s_entries[index].coalesce_key;
    }
    for (size_t i = index; i + 1 < s_count; ++i) {
        s_entries[i].callback.swap(s_entries[i + 1].callback);
        std::swap(s_entries[i].retained_bytes,
                  s_entries[i + 1].retained_bytes);
        std::swap(s_entries[i].coalesce_key,
                  s_entries[i + 1].coalesce_key);
    }
    --s_count;
    s_entries[s_count].retained_bytes = 0;
    s_entries[s_count].coalesce_key =
        static_cast<UiDispatcher::SnapshotKey>(0);
}

UiDispatcher::PostResult Enqueue(std::function<void()> callback,
                                 size_t retained_bytes,
                                 UiDispatcher::SnapshotKey coalesce_key) {
    if (!callback || !s_initialized.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(s_queue_mutex);
            ++s_diagnostics.rejected;
        }
        RecordRejected();
        return UiDispatcher::PostResult::Unavailable;
    }
    if (retained_bytes > kMaxQueuedBytes) {
        {
            std::lock_guard<std::mutex> lock(s_queue_mutex);
            ++s_diagnostics.rejected;
        }
        RecordRejected();
        return UiDispatcher::PostResult::ByteLimit;
    }

    UiDispatcher::PostResult result = UiDispatcher::PostResult::Accepted;
    Entry replaced_entry{};
    {
        std::unique_lock<std::mutex> lock(s_queue_mutex);
        size_t existing = s_count;
        if (coalesce_key != static_cast<UiDispatcher::SnapshotKey>(0)) {
            for (size_t i = 0; i < s_count; ++i) {
                if (s_entries[i].coalesce_key == coalesce_key) {
                    existing = i;
                    break;
                }
            }
        }

        const bool replacing = existing < s_count;
        const size_t retained_without_old = s_queued_bytes -
            (replacing ? s_entries[existing].retained_bytes : 0);
        if (retained_bytes > kMaxQueuedBytes - retained_without_old) {
            ++s_diagnostics.rejected;
            lock.unlock();
            RecordRejected();
            return UiDispatcher::PostResult::ByteLimit;
        }
        if (!replacing && s_count == s_entries.size()) {
            ++s_diagnostics.rejected;
            lock.unlock();
            RecordRejected();
            return UiDispatcher::PostResult::Full;
        }

        if (replacing) {
            EraseAt(existing, &replaced_entry);
            result = UiDispatcher::PostResult::Replaced;
        }
        s_entries[s_count++] = Entry{
            .callback = std::move(callback),
            .retained_bytes = retained_bytes,
            .coalesce_key = coalesce_key,
        };
        s_queued_bytes += retained_bytes;
        ++s_diagnostics.accepted;
        if (replacing) ++s_diagnostics.replaced;
        s_diagnostics.high_water_items =
            std::max(s_diagnostics.high_water_items, s_count);
        s_diagnostics.high_water_bytes =
            std::max(s_diagnostics.high_water_bytes, s_queued_bytes);
    }

    WakeExecutor();
    return result;
}

void DrainTimer(lv_timer_t*) {
    UiDispatcher::DrainPending();
}
}  // namespace

bool UiDispatcher::ConfigureExecutor(WakeCallback wake_callback) {
    if (wake_callback == nullptr) return false;
    std::lock_guard<std::mutex> setup_lock(s_setup_mutex);
    if (s_initialized.load(std::memory_order_acquire)) return false;
    s_wake_callback.store(wake_callback, std::memory_order_release);
    s_executor_configured = true;
    return true;
}

bool UiDispatcher::Init() {
    std::lock_guard<std::mutex> setup_lock(s_setup_mutex);
    if (s_initialized.load(std::memory_order_acquire)) return true;

    if (!s_executor_configured) {
        s_timer = lv_timer_create(DrainTimer, kDrainPeriodMs, nullptr);
        if (s_timer == nullptr) {
            ESP_LOGE(kTag, "Failed to create UI drain timer");
            return false;
        }
    }
    s_initialized.store(true, std::memory_order_release);
    return true;
}

bool UiDispatcher::Post(std::function<void()> callback) {
    const PostResult result = Enqueue(
        std::move(callback), kDefaultCallbackChargeBytes,
        static_cast<SnapshotKey>(0));
    return result == PostResult::Accepted;
}

UiDispatcher::PostResult UiDispatcher::PostBounded(
    std::function<void()> callback, size_t retained_bytes) {
    const PostResult result = Enqueue(
        std::move(callback), retained_bytes, static_cast<SnapshotKey>(0));
    return result;
}

UiDispatcher::PostResult UiDispatcher::PostLatest(
    SnapshotKey key, std::function<void()> callback, size_t retained_bytes) {
    if (key == static_cast<SnapshotKey>(0)) {
        {
            std::lock_guard<std::mutex> lock(s_queue_mutex);
            ++s_diagnostics.rejected;
        }
        RecordRejected();
        return PostResult::Unavailable;
    }
    const PostResult result = Enqueue(std::move(callback), retained_bytes, key);
    return result;
}

UiDispatcher::Diagnostics UiDispatcher::GetDiagnostics() {
    std::lock_guard<std::mutex> lock(s_queue_mutex);
    Diagnostics result = s_diagnostics;
    result.queued_items = s_count;
    result.queued_bytes = s_queued_bytes;
    return result;
}

bool UiDispatcher::DrainPending() {
    if (!s_initialized.load(std::memory_order_acquire)) return false;
    const int64_t start_us = esp_timer_get_time();
    size_t drained = 0;
    while (drained < kMaxTasksPerPass &&
           esp_timer_get_time() - start_us < kDrainBudgetUs) {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(s_queue_mutex);
            if (s_count == 0) break;
            callback.swap(s_entries[0].callback);
            s_queued_bytes -= s_entries[0].retained_bytes;
            for (size_t i = 0; i + 1 < s_count; ++i) {
                s_entries[i].callback.swap(s_entries[i + 1].callback);
                std::swap(s_entries[i].retained_bytes,
                          s_entries[i + 1].retained_bytes);
                std::swap(s_entries[i].coalesce_key,
                          s_entries[i + 1].coalesce_key);
            }
            --s_count;
            s_entries[s_count].retained_bytes = 0;
            s_entries[s_count].coalesce_key =
                static_cast<SnapshotKey>(0);
        }
        if (callback) callback();
        ++drained;
    }

    bool pending = false;
    {
        std::lock_guard<std::mutex> lock(s_queue_mutex);
        pending = s_count != 0;
    }
    if (pending) WakeExecutor();
    return pending;
}
