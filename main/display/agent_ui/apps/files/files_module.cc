#include "apps/files/files_module.h"

#include "core/navigation.h"

namespace agent_ui {

FilesModule::FilesModule() : controller_(adapter_) {
    controller_.SetStateChanged(
        [this](const files::ViewState& state) { OnStateChanged(state); });
}

FilesModule& FilesModule::Get() {
    static FilesModule module;
    return module;
}

lv_obj_t* FilesModule::Create() {
    lv_obj_t* screen = FilesView::CreateWidgets();
    if (screen != nullptr) {
        created_ = true;
        adapter_.InitUsb();
        adapter_.SetWorkerReadyCallback([] {
            FilesModule::Get().ApplyPendingIoResults();
        });
        adapter_.SetUsbNotify([] { FilesModule::Get().OnUsbStateChanged(); });
        adapter_.NotifyWorkerReady();
        controller_.Load();
        OnStateChanged(controller_.state());
    }
    return screen;
}

void FilesModule::LifecycleCallback(AppLifecycleEvent event) {
    if (event == AppLifecycleEvent::Load || event == AppLifecycleEvent::Resume) {
        adapter_.SetWorkerReadyCallback([] {
            FilesModule::Get().ApplyPendingIoResults();
        });
        adapter_.NotifyWorkerReady();
        adapter_.SetUsbNotify([] { FilesModule::Get().OnUsbStateChanged(); });
        if (event == AppLifecycleEvent::Resume || !created_) controller_.Resume();
        else controller_.RefreshStatus();
        FilesView::RenderState(controller_.state());
        return;
    }
    if (event == AppLifecycleEvent::Suspend) {
        ClosePreview();
        adapter_.SetUsbNotify({});
        controller_.Suspend();
        return;
    }

    ClosePreview();
    controller_.Unload();
    adapter_.SetUsbNotify({});
    adapter_.SetWorkerReadyCallback(nullptr);
    adapter_.DisableUsbIfActive();
    created_ = false;
    FilesView::OnUnloadWidgets();
}

void FilesModule::OnStateChanged(const files::ViewState& state) {
    FilesView::RenderState(state);
    if (processing_preview_request_) return;
    processing_preview_request_ = true;
    if (controller_.HasPendingImagePresentation()) {
        PresentPendingImage();
    } else if (controller_.HasPendingTextPreview()) {
        if (controller_.StartPendingTextPreview()) {
            FilesView::ClosePreviewVisual();
            adapter_.ReleaseImageLease();
            controller_.DropDisplayedPreviewForReplacement();
            controller_.DispatchPendingTextPreview();
        } else {
            controller_.AbortPendingTextPreview();
        }
    }
    processing_preview_request_ = false;
}

void FilesModule::OnUsbStateChanged() {
    FilesView::ScheduleUsbStateRefresh();
}

void FilesModule::PresentPendingImage() {
    const std::string path = controller_.pending_preview_path();
    if (path.empty()) return;
    // Drop the LVGL source and decoder caches before giving USB or another
    // local operation access to the SD card.
    FilesView::ClosePreviewVisual();
    adapter_.ReleaseImageLease();
    controller_.DropDisplayedPreviewForReplacement();
    const bool lease_acquired = adapter_.TryAcquireImageLease();
    const bool presented = lease_acquired && FilesView::ShowImagePreview(path.c_str());
    if (!controller_.CompleteImagePresentation(presented, true)) {
        FilesView::ClosePreviewVisual();
        adapter_.ReleaseImageLease();
    }
}

void FilesModule::NavigateBack() {
    if (controller_.state().preview_kind != files::PreviewKind::None) {
        ClosePreview();
        controller_.RefreshStatus();
        return;
    }
    if (!controller_.NavigateBack()) {
        adapter_.DisableUsbIfActive();
        Navigation::Get().Back();
    }
}

void FilesModule::ActivateEntry(std::size_t index, uint32_t row_epoch) {
    controller_.ActivateEntry(index, row_epoch);
}

void FilesModule::PreviousPage() { controller_.PreviousPage(); }

void FilesModule::NextPage() { controller_.NextPage(); }

void FilesModule::ToggleUsb() {
    if (adapter_.GetUsbState().busy) return;
    ClosePreview();
    adapter_.ToggleUsb();
}

void FilesModule::HandleUsbUiNotification() {
    if (adapter_.GetAvailability() != files::Availability::Ready &&
        (adapter_.HasImageLease() ||
         controller_.state().preview_kind != files::PreviewKind::None))
        ClosePreview();
    controller_.RefreshStatus();
    controller_.RefreshDirectory();
    controller_.RefreshCapacity();
}

void FilesModule::RefreshDirectory() { controller_.RefreshDirectory(); }

void FilesModule::ClosePreview() {
    FilesView::ClosePreviewVisual();
    controller_.ClosePreviewState();
    adapter_.ReleaseImageLease();
}

bool FilesModule::PreviewPath(
    const char* path, const files_io_worker::TicketPtr& ticket) {
    const bool accepted = controller_.PreviewPath(path, ticket);
    if (!accepted || path == nullptr) return false;
    if (controller_.state().preview_path != path) return false;
    const auto kind = controller_.state().preview_kind;
    if (kind == files::PreviewKind::Image) return controller_.IsPreviewFor(path);
    return kind == files::PreviewKind::TextLoading ||
           kind == files::PreviewKind::Text ||
           controller_.HasPendingTextPreview();
}

bool FilesModule::RequestDeletePath(
    const char* path, const files_io_worker::TicketPtr& ticket) {
    if (!controller_.CanDeletePath(path, ticket)) return false;
    if (adapter_.HasImageLease() || controller_.IsPreviewFor(path)) ClosePreview();
    return controller_.RequestDeletePath(path, ticket);
}

bool FilesModule::RequestDeletePreview(
    const files_io_worker::TicketPtr& ticket) {
    const std::string path = controller_.state().preview_path;
    if (path.empty() || !controller_.CanDeletePath(path.c_str(), ticket)) return false;
    ClosePreview();
    return controller_.RequestDeletePath(path.c_str(), ticket);
}

bool FilesModule::DeletePath(const char* path) {
    return controller_.DeletePathSync(path);
}

bool FilesModule::GetStorageBytes(uint64_t* total, uint64_t* free) {
    return controller_.ReadStorageBytes(total, free);
}

bool FilesModule::IsPreviewFor(const char* path) const {
    return controller_.IsPreviewFor(path);
}

void FilesModule::ApplyPendingIoResults() {
    controller_.ApplyPendingResults();
}

}  // namespace agent_ui
