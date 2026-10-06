#include "apps/files/files_io_worker.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <optional>
#include <utility>

#include "apps/files/files_directory_repository.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "SdCardManager.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_virtual_disk.h"
#include "ui_dispatcher.h"

namespace agent_ui::files_io_worker {
namespace {

constexpr size_t kWorkerStackBytes = 6144;
constexpr size_t kQueueCapacity = 6;
constexpr size_t kDirectoryPageEntries = 48;
constexpr size_t kDirectoryNameStorageBytes = 16 * 1024;
constexpr size_t kTextPreviewBytes = 48 * 1024;
constexpr size_t kMailboxRetainedBytes =
    kDirectoryNameStorageBytes + kTextPreviewBytes + 4096;
constexpr auto kCapacityLeaseRetryDelay = std::chrono::milliseconds(250);
constexpr char kTag[] = "FilesIo";
enum class Kind : uint8_t { Directory, Preview, Delete, Capacity };

struct Request {
    Kind kind = Kind::Directory;
    TicketPtr ticket;
    std::string path;
    std::string root;
    size_t offset = 0;
    uint32_t generation = 0;
    bool prestarted = false;
    uint32_t capacity_epoch = 0;
};

std::mutex s_service_mutex;
std::deque<std::shared_ptr<Request>> s_requests;
std::shared_ptr<Request> s_deferred_capacity;
std::chrono::steady_clock::time_point s_capacity_retry_at{};
TaskHandle_t s_worker = nullptr;
std::weak_ptr<Ticket> s_active_directory;
std::weak_ptr<Ticket> s_active_preview;
std::weak_ptr<Ticket> s_active_capacity;
TicketPtr s_last_capacity_ticket;
TicketPtr s_cached_capacity_ticket;
std::chrono::steady_clock::time_point s_capacity_attempt_time{};
std::chrono::steady_clock::time_point s_capacity_success_time{};
bool s_capacity_cache_valid = false;
uint32_t s_capacity_cache_epoch = 1;

struct Mailbox {
    std::mutex mutex;
    std::optional<DirectoryResult> directory;
    std::optional<PreviewResult> preview;
    std::optional<std::pair<uint64_t, bool>> deletion;
    bool notification_queued = false;
    void (*callback)() = nullptr;
};
Mailbox s_mailbox;
std::atomic<uint64_t> s_next_ticket{0};

void DispatchUiNotification() {
    void (*callback)() = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_mailbox.mutex);
        s_mailbox.notification_queued = false;
        callback = s_mailbox.callback;
    }
    if (callback != nullptr) callback();
}

void QueueUiNotification() {
    {
        std::lock_guard<std::mutex> lock(s_mailbox.mutex);
        if (s_mailbox.notification_queued) return;
        s_mailbox.notification_queued = true;
    }
    const auto result = UiDispatcher::PostBounded(
        [] { DispatchUiNotification(); }, kMailboxRetainedBytes);
    if (result != UiDispatcher::PostResult::Accepted &&
        result != UiDispatcher::PostResult::Replaced) {
        std::lock_guard<std::mutex> lock(s_mailbox.mutex);
        s_mailbox.notification_queued = false;
    }
}

bool Cancelled(void* context) {
    auto* ticket = static_cast<Ticket*>(context);
    return ticket == nullptr || ticket->IsCancellationRequested();
}

bool EnsureWorkerLocked() {
    if (s_worker != nullptr) return true;
    const auto worker_task = [](void*) {
            for (;;) {
                std::shared_ptr<Request> request;
                TickType_t wait_ticks = portMAX_DELAY;
                {
                    std::lock_guard<std::mutex> lock(s_service_mutex);
                    if (!s_requests.empty()) {
                        request = std::move(s_requests.front());
                        s_requests.pop_front();
                    } else if (s_deferred_capacity != nullptr) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now >= s_capacity_retry_at) {
                            request = std::move(s_deferred_capacity);
                            s_deferred_capacity.reset();
                        } else {
                            const auto remaining =
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    s_capacity_retry_at - now).count();
                            wait_ticks = pdMS_TO_TICKS(static_cast<uint32_t>(
                                std::max<int64_t>(1, remaining)));
                        }
                    }
                    if (request != nullptr) {
                        if (request->kind == Kind::Directory)
                            s_active_directory = request->ticket;
                        else if (request->kind == Kind::Preview)
                            s_active_preview = request->ticket;
                        else if (request->kind == Kind::Capacity)
                            s_active_capacity = request->ticket;
                    }
                }
                if (request == nullptr) {
                    ulTaskNotifyTake(pdTRUE, wait_ticks);
                    continue;
                }

                    auto& disk = UsbVirtualDisk::GetInstance();
                    const bool initially_available =
                        SdCardManager::GetInstance().IsMounted() &&
                        !disk.IsSdExportedToHost() && !disk.IsBusy();
                    UsbVirtualDisk::SdLocalAccess access(disk);
                    const bool acquired = initially_available && access.acquired() &&
                        SdCardManager::GetInstance().IsMounted() &&
                        !disk.IsSdExportedToHost() && !disk.IsBusy();

                    if (request->kind == Kind::Directory) {
                        DirectoryResult result;
                        result.ticket_id = request->ticket->id();
                        result.generation = request->generation;
                        result.directory = request->path;
                        if (acquired && request->ticket->TryStart() &&
                            !request->ticket->IsCancellationRequested()) {
                            files_directory_repository::DirectoryReadOptions options;
                            options.max_entries = kDirectoryPageEntries;
                            options.offset = request->offset;
                            options.max_name_bytes = 255;
                            options.max_storage_bytes = kDirectoryNameStorageBytes;
                            options.is_cancelled = Cancelled;
                            options.cancel_context = request->ticket.get();
                            options.truncated = &result.truncated;
                            options.had_unaddressable_entries =
                                &result.had_unaddressable_entries;
                            const auto* mount = SdCardManager::GetInstance().GetMountPoint();
                            result.status = files_directory_repository::ReadDirectory(
                                request->path.c_str(),
                                request->root.empty()
                                    ? (mount != nullptr ? mount : "/sdcard")
                                    : request->root.c_str(),
                                result.entries, options);
                        }
                        const bool success = acquired &&
                            result.status != files_directory_repository::DirectoryReadResult::Unavailable &&
                            result.status != files_directory_repository::DirectoryReadResult::Cancelled;
                        {
                            std::lock_guard<std::mutex> lock(s_mailbox.mutex);
                            s_mailbox.directory.emplace(std::move(result));
                        }
                        request->ticket->Finish(success);
                        QueueUiNotification();
                    } else if (request->kind == Kind::Preview) {
                        PreviewResult result;
                        result.ticket_id = request->ticket->id();
                        result.generation = request->generation;
                        result.path = request->path;
                        const bool started = request->prestarted
                            ? request->ticket->phase() == Ticket::Phase::Started
                            : request->ticket->TryStart();
                        if (acquired && started &&
                            !request->ticket->IsCancellationRequested()) {
                            result.status = files_directory_repository::ReadTextPreview(
                                request->path.c_str(), kTextPreviewBytes, &result.preview,
                                Cancelled, request->ticket.get());
                        }
                        const bool success = acquired && result.status ==
                            files_directory_repository::TextPreviewReadResult::Success;
                        {
                            std::lock_guard<std::mutex> lock(s_mailbox.mutex);
                            s_mailbox.preview.emplace(std::move(result));
                        }
                        request->ticket->Finish(success);
                        QueueUiNotification();
                    } else if (request->kind == Kind::Delete) {
                        const bool success = acquired && request->ticket->TryStart() &&
                            files_directory_repository::RemoveFile(request->path.c_str());
                        {
                            std::lock_guard<std::mutex> lock(s_mailbox.mutex);
                            s_mailbox.deletion =
                                std::make_pair(request->ticket->id(), success);
                        }
                        request->ticket->Finish(success);
                        QueueUiNotification();
                    } else {
                        uint64_t total_bytes = 0;
                        uint64_t free_bytes = 0;
                        const bool started = acquired && request->ticket->TryStart();
                        const bool read_success = started &&
                            files_directory_repository::ReadStorageBytes(
                                &total_bytes, &free_bytes);
                        const bool success = read_success &&
                            !request->ticket->IsCancellationRequested();
                        if (success) request->ticket->SetCapacity(total_bytes, free_bytes);
                        if (success) {
                            std::lock_guard<std::mutex> lock(s_service_mutex);
                            if (request->capacity_epoch == s_capacity_cache_epoch &&
                                !request->ticket->IsCancellationRequested()) {
                                s_cached_capacity_ticket = request->ticket;
                                s_capacity_cache_valid = true;
                                s_capacity_success_time =
                                    std::chrono::steady_clock::now();
                            }
                        }
                        bool deferred = false;
                        if (!success && !acquired &&
                                   request->ticket->phase() == Ticket::Phase::Queued &&
                                   SdCardManager::GetInstance().IsMounted() &&
                                   !disk.IsSdExportedToHost() && !disk.IsBusy()) {
                            // A held image-preview lease is local ownership,
                            // not unavailable media. Retain this one ticket and
                            // retry from the worker without blocking other jobs
                            // or entering FatFs until the lease can be acquired.
                            std::lock_guard<std::mutex> lock(s_service_mutex);
                            if (s_deferred_capacity == nullptr &&
                                request->ticket->phase() == Ticket::Phase::Queued) {
                                s_deferred_capacity = request;
                                s_capacity_retry_at =
                                    std::chrono::steady_clock::now() +
                                    kCapacityLeaseRetryDelay;
                                deferred = true;
                            }
                        }
                        if (!deferred) request->ticket->Finish(success);
                        if (!deferred) QueueUiNotification();
                    }

                    {
                        std::lock_guard<std::mutex> lock(s_service_mutex);
                        if (request->kind == Kind::Directory) s_active_directory.reset();
                        else if (request->kind == Kind::Preview) s_active_preview.reset();
                        else if (request->kind == Kind::Capacity &&
                                 s_deferred_capacity != request) {
                            s_active_capacity.reset();
                        }
                    }
            }
        };
    BaseType_t created = xTaskCreateWithCaps(
        worker_task, "files_io", kWorkerStackBytes, nullptr, 3, &s_worker,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(kTag, "PSRAM worker stack unavailable; trying internal RAM");
        created = xTaskCreateWithCaps(
            worker_task, "files_io", kWorkerStackBytes, nullptr, 3, &s_worker,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (created != pdPASS) {
        s_worker = nullptr;
        return false;
    }
    return true;
}

void CancelAndRemoveQueuedLocked(Kind kind) {
    for (auto it = s_requests.begin(); it != s_requests.end();) {
        if ((*it)->kind == kind) {
            (*it)->ticket->CancelBeforeStart();
            it = s_requests.erase(it);
        } else {
            ++it;
        }
    }
    if (kind == Kind::Capacity && s_deferred_capacity != nullptr) {
        s_deferred_capacity->ticket->CancelBeforeStart();
        s_deferred_capacity.reset();
    }
    std::weak_ptr<Ticket>* active = nullptr;
    if (kind == Kind::Directory) active = &s_active_directory;
    else if (kind == Kind::Preview) active = &s_active_preview;
    else if (kind == Kind::Capacity) active = &s_active_capacity;
    if (active != nullptr) {
        if (auto ticket = active->lock()) ticket->RequestCancellation();
    }
}

bool Enqueue(const std::shared_ptr<Request>& request, bool replace_kind) {
    std::lock_guard<std::mutex> lock(s_service_mutex);
    if (!EnsureWorkerLocked()) return false;
    if (replace_kind) CancelAndRemoveQueuedLocked(request->kind);
    if (s_requests.size() >= kQueueCapacity || !request->ticket->Queue()) return false;
    s_requests.push_back(request);
    xTaskNotifyGive(s_worker);
    return true;
}

bool ValidPath(const char* path) {
    return path != nullptr && path[0] != '\0' &&
           std::strlen(path) < files_directory_repository::kMaxPathLength;
}

void StoreFailedDelete(const TicketPtr& ticket) {
    ticket->Finish(false);
    {
        std::lock_guard<std::mutex> lock(s_mailbox.mutex);
        s_mailbox.deletion = std::make_pair(ticket->id(), false);
    }
    QueueUiNotification();
}

}  // namespace

bool Ticket::IsDone() const {
    const Phase current = phase();
    return current == Phase::Cancelled || current == Phase::Complete;
}

bool Ticket::Queue() {
    Phase expected = Phase::NotAdmitted;
    return phase_.compare_exchange_strong(expected, Phase::Queued,
                                          std::memory_order_acq_rel);
}

bool Ticket::TryStart() {
    Phase expected = Phase::Queued;
    return phase_.compare_exchange_strong(expected, Phase::Started,
                                          std::memory_order_acq_rel);
}

bool Ticket::CancelBeforeStart() {
    Phase current = phase();
    while (current == Phase::NotAdmitted || current == Phase::Queued) {
        if (phase_.compare_exchange_weak(current, Phase::Cancelled,
                                         std::memory_order_acq_rel)) {
            cancel_requested_.store(true, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void Ticket::RequestCancellation() {
    cancel_requested_.store(true, std::memory_order_release);
    CancelBeforeStart();
}

void Ticket::Finish(bool succeeded) {
    succeeded_.store(succeeded, std::memory_order_release);
    Phase current = phase();
    while (current != Phase::Cancelled && current != Phase::Complete) {
        if (phase_.compare_exchange_weak(current, Phase::Complete,
                                         std::memory_order_acq_rel)) break;
    }
}

void Ticket::SetCapacity(uint64_t total_bytes, uint64_t free_bytes) {
    std::lock_guard<std::mutex> lock(capacity_mutex_);
    total_bytes_ = total_bytes;
    free_bytes_ = free_bytes;
}

bool Ticket::GetCapacity(uint64_t* total_bytes, uint64_t* free_bytes) const {
    if (total_bytes == nullptr || free_bytes == nullptr || !Succeeded()) return false;
    std::lock_guard<std::mutex> lock(capacity_mutex_);
    *total_bytes = total_bytes_;
    *free_bytes = free_bytes_;
    return true;
}

void SetUiCallback(void (*callback)()) {
    std::lock_guard<std::mutex> lock(s_mailbox.mutex);
    s_mailbox.callback = callback;
}

TicketPtr CreateTicket() {
    return std::make_shared<Ticket>(s_next_ticket.fetch_add(1) + 1);
}

bool SubmitDirectory(const TicketPtr& ticket, const char* directory,
                     const char* root, std::size_t offset,
                     uint32_t generation) {
    if (ticket == nullptr || !ValidPath(directory) ||
        (root != nullptr && !ValidPath(root))) return false;
    auto request = std::make_shared<Request>();
    request->kind = Kind::Directory;
    request->ticket = ticket;
    request->path = directory;
    if (root != nullptr) request->root = root;
    request->offset = offset;
    request->generation = generation;
    const bool accepted = Enqueue(request, true);
    if (!accepted) ticket->Finish(false);
    return accepted;
}

bool SubmitPreview(const TicketPtr& ticket, const char* path,
                   uint32_t generation) {
    if (ticket == nullptr || !ValidPath(path)) return false;
    auto request = std::make_shared<Request>();
    request->kind = Kind::Preview;
    request->ticket = ticket;
    request->path = path;
    request->generation = generation;
    const bool accepted = Enqueue(request, true);
    if (!accepted) ticket->Finish(false);
    return accepted;
}

bool SubmitPreviewStarted(const TicketPtr& ticket, const char* path,
                          uint32_t generation) {
    if (ticket == nullptr || !ValidPath(path) ||
        ticket->phase() != Ticket::Phase::Started ||
        ticket->IsCancellationRequested()) return false;
    auto request = std::make_shared<Request>();
    request->kind = Kind::Preview;
    request->ticket = ticket;
    request->path = path;
    request->generation = generation;
    request->prestarted = true;
    {
        std::lock_guard<std::mutex> lock(s_service_mutex);
        if (!EnsureWorkerLocked()) {
            ticket->Finish(false);
            return false;
        }
        CancelAndRemoveQueuedLocked(Kind::Preview);
        if (s_requests.size() >= kQueueCapacity ||
            ticket->phase() != Ticket::Phase::Started ||
            ticket->IsCancellationRequested()) {
            ticket->Finish(false);
            return false;
        }
        s_requests.push_back(request);
        xTaskNotifyGive(s_worker);
    }
    return true;
}

bool SubmitDelete(const TicketPtr& ticket, const char* path,
                  uint32_t generation) {
    if (ticket == nullptr || !ValidPath(path)) return false;
    if (ticket->phase() == Ticket::Phase::Cancelled) return false;
    auto request = std::make_shared<Request>();
    request->kind = Kind::Delete;
    request->ticket = ticket;
    request->path = path;
    request->generation = generation;
    if (Enqueue(request, false)) return true;
    StoreFailedDelete(ticket);
    return false;
}

TicketPtr RequestCapacity(uint32_t generation,
                          const TicketPtr& admission_ticket) {
    if (admission_ticket != nullptr &&
        admission_ticket->phase() != Ticket::Phase::NotAdmitted) {
        return admission_ticket;
    }
    auto& disk = UsbVirtualDisk::GetInstance();
    const bool storage_available = SdCardManager::GetInstance().IsMounted() &&
        !disk.IsSdExportedToHost() && !disk.IsBusy();
    std::lock_guard<std::mutex> lock(s_service_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!storage_available) {
        ++s_capacity_cache_epoch;
        CancelAndRemoveQueuedLocked(Kind::Capacity);
        s_capacity_cache_valid = false;
        s_cached_capacity_ticket.reset();
        auto failed = admission_ticket != nullptr ? admission_ticket : CreateTicket();
        failed->Finish(false);
        s_last_capacity_ticket = failed;
        s_capacity_attempt_time = now;
        return failed;
    }
    if (auto current = s_active_capacity.lock(); current && !current->IsDone())
        return current;
    if (s_capacity_cache_valid && s_cached_capacity_ticket != nullptr &&
        s_cached_capacity_ticket->Succeeded() &&
        now - s_capacity_success_time < std::chrono::seconds(5)) {
        s_last_capacity_ticket = s_cached_capacity_ticket;
        return s_cached_capacity_ticket;
    }
    if (auto previous = s_last_capacity_ticket) {
        if (!previous->IsDone()) return previous;
        if (!previous->Succeeded() &&
            now - s_capacity_attempt_time < std::chrono::seconds(2)) return previous;
    }

    auto ticket = admission_ticket != nullptr ? admission_ticket : CreateTicket();
    if (!EnsureWorkerLocked()) {
        ticket->Finish(false);
        s_last_capacity_ticket = ticket;
        s_capacity_attempt_time = now;
        return ticket;
    }
    if (s_requests.size() >= kQueueCapacity || !ticket->Queue()) {
        ticket->Finish(false);
        s_last_capacity_ticket = ticket;
        s_capacity_attempt_time = now;
        return ticket;
    }
    auto request = std::make_shared<Request>();
    request->kind = Kind::Capacity;
    request->ticket = ticket;
    request->generation = generation;
    request->capacity_epoch = s_capacity_cache_epoch;
    s_requests.push_back(request);
    s_last_capacity_ticket = ticket;
    s_capacity_attempt_time = now;
    s_capacity_cache_valid = false;
    xTaskNotifyGive(s_worker);
    return ticket;
}

void InvalidateCapacityCache() {
    std::lock_guard<std::mutex> lock(s_service_mutex);
    ++s_capacity_cache_epoch;
    s_capacity_cache_valid = false;
    s_cached_capacity_ticket.reset();
    s_capacity_attempt_time = {};
}

bool TakeDirectory(uint64_t ticket_id, DirectoryResult* result) {
    if (result == nullptr) return false;
    std::lock_guard<std::mutex> lock(s_mailbox.mutex);
    if (!s_mailbox.directory || s_mailbox.directory->ticket_id != ticket_id) return false;
    *result = std::move(*s_mailbox.directory);
    s_mailbox.directory.reset();
    return true;
}

bool TakePreview(uint64_t ticket_id, PreviewResult* result) {
    if (result == nullptr) return false;
    std::lock_guard<std::mutex> lock(s_mailbox.mutex);
    if (!s_mailbox.preview || s_mailbox.preview->ticket_id != ticket_id) return false;
    *result = std::move(*s_mailbox.preview);
    s_mailbox.preview.reset();
    return true;
}

bool TakeDelete(uint64_t ticket_id, bool* succeeded) {
    if (succeeded == nullptr) return false;
    std::lock_guard<std::mutex> lock(s_mailbox.mutex);
    if (!s_mailbox.deletion || s_mailbox.deletion->first != ticket_id) return false;
    *succeeded = s_mailbox.deletion->second;
    s_mailbox.deletion.reset();
    return true;
}

void NotifyUiPending() { QueueUiNotification(); }

}  // namespace agent_ui::files_io_worker
