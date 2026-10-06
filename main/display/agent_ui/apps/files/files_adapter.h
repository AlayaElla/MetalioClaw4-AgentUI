#pragma once

#include <memory>

#include "apps/files/files_controller.h"
#include "usb_virtual_disk.h"

namespace agent_ui::files {

// Board/worker boundary. Hardware state and the long-lived image lease live
// here, never in Controller or ViewState.
class FilesAdapter final : public Port {
public:
    Availability GetAvailability() const override;
    std::string GetRootPath() const override;
    bool SubmitDirectory(const files_io_worker::TicketPtr& ticket,
                         const char* directory, const char* root,
                         std::size_t offset, uint32_t generation) override;
    bool SubmitPreview(const files_io_worker::TicketPtr& ticket,
                       const char* path, uint32_t generation) override;
    bool StartPreparedPreview(
        const files_io_worker::TicketPtr& ticket) override;
    bool DispatchPreparedPreview(const files_io_worker::TicketPtr& ticket,
                                 const char* path,
                                 uint32_t generation) override;
    void CancelPreparedPreview(
        const files_io_worker::TicketPtr& ticket) override;
    bool SubmitDelete(const files_io_worker::TicketPtr& ticket,
                      const char* path, uint32_t generation) override;
    files_io_worker::TicketPtr RequestCapacity(uint32_t generation) override;
    void InvalidateCapacity() override;
    bool TakeDirectory(uint64_t id,
                       files_io_worker::DirectoryResult* result) override;
    bool TakePreview(uint64_t id,
                     files_io_worker::PreviewResult* result) override;
    bool TakeDelete(uint64_t id, bool* succeeded) override;
    bool TryAcquireImageLease();
    void ReleaseImageLease();
    bool HasImageLease() const;
    bool DeletePathSync(const char* path) override;
    bool ReadStorageBytes(uint64_t* total, uint64_t* free) override;
    bool GetCapacityFallback(uint64_t* total, uint64_t* free) const override;
    UsbState GetUsbState() const;
    void SetUsbNotify(std::function<void()> notify);
    void ToggleUsb();
    void DisableUsbIfActive();

    void SetWorkerReadyCallback(void (*callback)());
    void NotifyWorkerReady();
    void InitUsb();

private:
    std::unique_ptr<UsbVirtualDisk::SdLocalAccess> image_lease_;
};

}  // namespace agent_ui::files
