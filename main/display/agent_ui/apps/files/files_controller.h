#pragma once

#include <functional>
#include <memory>

#include "apps/files/files_contract.h"
#include "apps/files/files_io_worker.h"

namespace agent_ui::files {

class Port {
public:
    virtual ~Port() = default;
    virtual Availability GetAvailability() const = 0;
    virtual std::string GetRootPath() const = 0;
    virtual bool SubmitDirectory(const files_io_worker::TicketPtr& ticket,
                                 const char* directory, const char* root,
                                 std::size_t offset, uint32_t generation) = 0;
    // Prepare admission only. The caller wins a queued-ticket transition
    // before it replaces any displayed preview or releases an image lease.
    virtual bool SubmitPreview(const files_io_worker::TicketPtr& ticket,
                               const char* path, uint32_t generation) = 0;
    virtual bool StartPreparedPreview(
        const files_io_worker::TicketPtr& ticket) = 0;
    virtual bool DispatchPreparedPreview(
        const files_io_worker::TicketPtr& ticket, const char* path,
        uint32_t generation) = 0;
    virtual void CancelPreparedPreview(
        const files_io_worker::TicketPtr& ticket) = 0;
    virtual bool SubmitDelete(const files_io_worker::TicketPtr& ticket,
                              const char* path, uint32_t generation) = 0;
    virtual files_io_worker::TicketPtr RequestCapacity(uint32_t generation) = 0;
    virtual void InvalidateCapacity() = 0;
    virtual bool TakeDirectory(uint64_t id,
                               files_io_worker::DirectoryResult* result) = 0;
    virtual bool TakePreview(uint64_t id,
                             files_io_worker::PreviewResult* result) = 0;
    virtual bool TakeDelete(uint64_t id, bool* succeeded) = 0;
    virtual bool DeletePathSync(const char* path) = 0;
    virtual bool ReadStorageBytes(uint64_t* total, uint64_t* free) = 0;
    virtual bool GetCapacityFallback(uint64_t* total, uint64_t* free) const = 0;
};

class Controller {
public:
    explicit Controller(Port& port);

    const ViewState& state() const { return state_; }
    void SetStateChanged(std::function<void(const ViewState&)> callback);

    void Load();
    void Resume();
    void Suspend();
    void Unload();
    void RefreshStatus();
    void RefreshDirectory();
    void RefreshCapacity();
    void ApplyPendingResults();

    // Returns false only when the current directory is root. Path/page changes
    // are controller-owned and immediately create a fresh directory request.
    bool NavigateBack();
    void ActivateEntry(std::size_t index, uint32_t row_epoch);
    void PreviousPage();
    void NextPage();
    bool PreviewPath(const char* path,
                     const files_io_worker::TicketPtr& ticket = nullptr);
    bool CanPreviewPath(const char* path,
                        const files_io_worker::TicketPtr& ticket = nullptr) const;
    bool RequestDeletePath(
        const char* path, const files_io_worker::TicketPtr& ticket = nullptr);
    bool CanDeletePath(const char* path,
                       const files_io_worker::TicketPtr& ticket) const;
    bool DeletePathSync(const char* path);
    bool ReadStorageBytes(uint64_t* total, uint64_t* free);
    bool IsPreviewFor(const char* path) const;
    const std::string& pending_preview_path() const {
        return pending_preview_path_;
    }
    bool HasPendingImagePresentation() const;
    bool HasPendingTextPreview() const;
    bool StartPendingTextPreview();
    bool DispatchPendingTextPreview();
    void AbortPendingTextPreview();
    void DropDisplayedPreviewForReplacement();
    bool CompleteImagePresentation(bool presented,
                                   bool previous_preview_cleared = false);
    void ClosePreviewState();

private:
    void NotifyStateChanged();
    void SetStatusFromAvailability(Availability availability);
    void CancelDirectoryRequest();
    void CancelPreviewRequest();
    void CancelDeleteRequest();
    void RequestDirectory();
    void ApplyDirectoryResult(files_io_worker::DirectoryResult&& result,
                              const files_io_worker::TicketPtr& ticket);
    void ApplyPreviewResult(files_io_worker::PreviewResult&& result,
                            const files_io_worker::TicketPtr& ticket);
    bool IsValidPath(const char* path) const;
    bool IsPreviewablePath(const char* path, bool* image) const;
    static uint32_t NextGeneration(uint32_t current);

    Port& port_;
    ViewState state_;
    std::function<void(const ViewState&)> state_changed_;
    files_io_worker::TicketPtr directory_ticket_;
    files_io_worker::TicketPtr preview_ticket_;
    files_io_worker::TicketPtr delete_ticket_;
    files_io_worker::TicketPtr capacity_ticket_;
    files_io_worker::TicketPtr image_ticket_;
    std::string pending_preview_path_;
    uint32_t pending_preview_generation_ = 0;
    bool preview_dispatched_ = false;
    uint32_t next_preview_generation_ = 1;
    std::string delete_path_;
    uint32_t delete_generation_ = 0;
    Availability last_availability_ = Availability::Ready;
    bool availability_known_ = false;
};

}  // namespace agent_ui::files
