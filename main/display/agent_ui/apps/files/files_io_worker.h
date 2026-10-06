#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "apps/files/files_directory_repository.h"

namespace agent_ui::files_io_worker {

class Ticket {
public:
    enum class Phase : uint8_t {
        NotAdmitted,
        Queued,
        Started,
        Cancelled,
        Complete,
    };

    explicit Ticket(uint64_t id) : id_(id) {}
    uint64_t id() const { return id_; }
    Phase phase() const { return phase_.load(std::memory_order_acquire); }
    bool IsDone() const;
    bool Succeeded() const { return succeeded_.load(std::memory_order_acquire); }
    bool Queue();
    bool TryStart();
    bool CancelBeforeStart();
    void RequestCancellation();
    bool IsCancellationRequested() const {
        return cancel_requested_.load(std::memory_order_acquire) ||
               phase() == Phase::Cancelled;
    }
    void Finish(bool succeeded);
    void SetCapacity(uint64_t total_bytes, uint64_t free_bytes);
    bool GetCapacity(uint64_t* total_bytes, uint64_t* free_bytes) const;

private:
    uint64_t id_ = 0;
    std::atomic<Phase> phase_{Phase::NotAdmitted};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> succeeded_{false};
    mutable std::mutex capacity_mutex_;
    uint64_t total_bytes_ = 0;
    uint64_t free_bytes_ = 0;
};

using TicketPtr = std::shared_ptr<Ticket>;

struct DirectoryResult {
    uint64_t ticket_id = 0;
    uint32_t generation = 0;
    std::string directory;
    files_directory_repository::DirectoryReadResult status =
        files_directory_repository::DirectoryReadResult::Unavailable;
    std::vector<files_directory_repository::Entry> entries;
    bool truncated = false;
    bool had_unaddressable_entries = false;
};

struct PreviewResult {
    uint64_t ticket_id = 0;
    uint32_t generation = 0;
    std::string path;
    files_directory_repository::TextPreviewReadResult status =
        files_directory_repository::TextPreviewReadResult::OpenFailed;
    files_directory_repository::TextPreview preview;
};

void SetUiCallback(void (*callback)());
TicketPtr CreateTicket();
bool SubmitDirectory(const TicketPtr& ticket, const char* directory,
                     const char* root, std::size_t offset,
                     uint32_t generation);
bool SubmitPreview(const TicketPtr& ticket, const char* path,
                   uint32_t generation);
// Enqueue a preview whose NotAdmitted -> Queued -> Started transition was
// already performed by the caller before replacing an on-screen image lease.
bool SubmitPreviewStarted(const TicketPtr& ticket, const char* path,
                          uint32_t generation);
bool SubmitDelete(const TicketPtr& ticket, const char* path,
                  uint32_t generation);
// A caller may supply a ticket when its admission needs to be cancellable
// before this service queues work. When fresh/shared work already exists, the
// returned ticket can differ and the supplied ticket remains unqueued.
TicketPtr RequestCapacity(uint32_t generation,
                          const TicketPtr& admission_ticket = nullptr);
// Drop only the reusable snapshot. Existing shared requests continue and keep
// their own result tickets; Files page lifecycle must not cancel them.
void InvalidateCapacityCache();

bool TakeDirectory(uint64_t ticket_id, DirectoryResult* result);
bool TakePreview(uint64_t ticket_id, PreviewResult* result);
bool TakeDelete(uint64_t ticket_id, bool* succeeded);
void NotifyUiPending();

}  // namespace agent_ui::files_io_worker
