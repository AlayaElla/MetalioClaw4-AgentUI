#pragma once

#include <cstdint>

#include "apps/files/files_adapter.h"
#include "apps/files/files_controller.h"
#include "apps/files/files_view.h"

namespace agent_ui {

class FilesModule {
public:
    static FilesModule& Get();
    lv_obj_t* Create();
    void LifecycleCallback(AppLifecycleEvent event);

    const files::ViewState& state() const { return controller_.state(); }
    files::Controller& controller() { return controller_; }
    files::UsbState usb_state() const { return adapter_.GetUsbState(); }

    void NavigateBack();
    void ActivateEntry(std::size_t index, uint32_t row_epoch);
    void PreviousPage();
    void NextPage();
    void ToggleUsb();
    void HandleUsbUiNotification();
    void RefreshDirectory();
    void ClosePreview();
    bool PreviewPath(const char* path,
                     const files_io_worker::TicketPtr& ticket = nullptr);
    bool RequestDeletePath(const char* path,
                           const files_io_worker::TicketPtr& ticket = nullptr);
    bool RequestDeletePreview(const files_io_worker::TicketPtr& ticket);
    bool DeletePath(const char* path);
    bool GetStorageBytes(uint64_t* total, uint64_t* free);
    bool IsPreviewFor(const char* path) const;
    void ApplyPendingIoResults();

private:
    FilesModule();
    void OnStateChanged(const files::ViewState& state);
    void OnUsbStateChanged();
    void PresentPendingImage();

    files::FilesAdapter adapter_;
    files::Controller controller_;
    bool created_ = false;
    bool processing_preview_request_ = false;
};

}  // namespace agent_ui
