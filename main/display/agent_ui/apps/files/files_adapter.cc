#include "apps/files/files_adapter.h"

#include "SdCardManager.hpp"
#include "apps/files/files_directory_repository.h"
#include "apps/files/files_io_worker.h"

namespace agent_ui::files {

Availability FilesAdapter::GetAvailability() const {
    auto& disk = UsbVirtualDisk::GetInstance();
    if (disk.IsBusy()) return Availability::UsbBusy;
    if (disk.IsSdExportedToHost()) return Availability::UsbExported;
    if (!SdCardManager::GetInstance().IsMounted()) return Availability::Missing;
    return Availability::Ready;
}

std::string FilesAdapter::GetRootPath() const {
    const char* mount = SdCardManager::GetInstance().GetMountPoint();
    return mount != nullptr ? mount : "/sdcard";
}

bool FilesAdapter::SubmitDirectory(const files_io_worker::TicketPtr& ticket,
                                   const char* directory, const char* root,
                                   std::size_t offset, uint32_t generation) {
    return GetAvailability() == Availability::Ready &&
           files_io_worker::SubmitDirectory(ticket, directory, root, offset,
                                             generation);
}

bool FilesAdapter::SubmitPreview(const files_io_worker::TicketPtr& ticket,
                                 const char* path, uint32_t generation) {
    (void)path;
    (void)generation;
    return ticket != nullptr && GetAvailability() == Availability::Ready &&
           ticket->Queue();
}

bool FilesAdapter::StartPreparedPreview(
    const files_io_worker::TicketPtr& ticket) {
    return ticket != nullptr && GetAvailability() == Availability::Ready &&
           ticket->TryStart();
}

bool FilesAdapter::DispatchPreparedPreview(
    const files_io_worker::TicketPtr& ticket, const char* path,
    uint32_t generation) {
    return GetAvailability() == Availability::Ready &&
           files_io_worker::SubmitPreviewStarted(ticket, path, generation);
}

void FilesAdapter::CancelPreparedPreview(
    const files_io_worker::TicketPtr& ticket) {
    if (ticket != nullptr) ticket->CancelBeforeStart();
}

bool FilesAdapter::SubmitDelete(const files_io_worker::TicketPtr& ticket,
                                const char* path, uint32_t generation) {
    return GetAvailability() == Availability::Ready &&
           files_io_worker::SubmitDelete(ticket, path, generation);
}

files_io_worker::TicketPtr FilesAdapter::RequestCapacity(uint32_t generation) {
    if (GetAvailability() != Availability::Ready) return nullptr;
    return files_io_worker::RequestCapacity(generation);
}

void FilesAdapter::InvalidateCapacity() {
    files_io_worker::InvalidateCapacityCache();
}

bool FilesAdapter::TakeDirectory(uint64_t id,
                                 files_io_worker::DirectoryResult* result) {
    return files_io_worker::TakeDirectory(id, result);
}

bool FilesAdapter::TakePreview(uint64_t id,
                               files_io_worker::PreviewResult* result) {
    return files_io_worker::TakePreview(id, result);
}

bool FilesAdapter::TakeDelete(uint64_t id, bool* succeeded) {
    return files_io_worker::TakeDelete(id, succeeded);
}

bool FilesAdapter::TryAcquireImageLease() {
    if (image_lease_ != nullptr || GetAvailability() != Availability::Ready)
        return false;
    auto& disk = UsbVirtualDisk::GetInstance();
    auto lease = std::make_unique<UsbVirtualDisk::SdLocalAccess>(disk);
    if (!lease->acquired() || GetAvailability() != Availability::Ready) return false;
    image_lease_ = std::move(lease);
    return true;
}

void FilesAdapter::ReleaseImageLease() { image_lease_.reset(); }

bool FilesAdapter::HasImageLease() const { return image_lease_ != nullptr; }

bool FilesAdapter::DeletePathSync(const char* path) {
    if (GetAvailability() != Availability::Ready || path == nullptr) return false;
    auto& disk = UsbVirtualDisk::GetInstance();
    UsbVirtualDisk::SdLocalAccess lease(disk);
    return lease.acquired() && GetAvailability() == Availability::Ready &&
           files_directory_repository::RemoveFile(path);
}

bool FilesAdapter::ReadStorageBytes(uint64_t* total, uint64_t* free) {
    if (total == nullptr || free == nullptr ||
        GetAvailability() != Availability::Ready) return false;
    auto& disk = UsbVirtualDisk::GetInstance();
    UsbVirtualDisk::SdLocalAccess lease(disk);
    return lease.acquired() && GetAvailability() == Availability::Ready &&
           files_directory_repository::ReadStorageBytes(total, free);
}

bool FilesAdapter::GetCapacityFallback(uint64_t* total, uint64_t* free) const {
    if (total == nullptr || free == nullptr ||
        GetAvailability() != Availability::Ready) return false;
    auto& disk = UsbVirtualDisk::GetInstance();
    UsbVirtualDisk::SdLocalAccess lease(disk);
    if (!lease.acquired() || GetAvailability() != Availability::Ready) return false;
    sdmmc_card_t* card = SdCardManager::GetInstance().GetCard();
    if (card == nullptr) return false;
    *total = static_cast<uint64_t>(card->csd.capacity) * card->csd.sector_size;
    *free = 0;
    return true;
}

UsbState FilesAdapter::GetUsbState() const {
    auto& disk = UsbVirtualDisk::GetInstance();
    return {disk.IsSupported(), disk.IsGadgetActive(), disk.IsBusy()};
}

void FilesAdapter::SetUsbNotify(std::function<void()> notify) {
    UsbVirtualDisk::GetInstance().SetUiNotify(std::move(notify));
}

void FilesAdapter::ToggleUsb() { UsbVirtualDisk::GetInstance().Toggle(); }

void FilesAdapter::DisableUsbIfActive() {
    UsbVirtualDisk::GetInstance().DisableIfActive();
}

void FilesAdapter::SetWorkerReadyCallback(void (*callback)()) {
    files_io_worker::SetUiCallback(callback);
}

void FilesAdapter::NotifyWorkerReady() { files_io_worker::NotifyUiPending(); }

void FilesAdapter::InitUsb() { UsbVirtualDisk::GetInstance().Init(); }

}  // namespace agent_ui::files
