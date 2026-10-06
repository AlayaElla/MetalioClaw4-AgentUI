#pragma once

#include <cstddef>
#include <cstdint>

#include "lvgl.h"
#include "core/ui_utils.h"
#include "apps/files/files_contract.h"
#include "apps/files/files_io_worker.h"

namespace agent_ui {

// FilesView owns LVGL widgets and visual presentation. Browser paths, preview
// admission, deletion, and SD/USB access are delegated to FilesModule.
class FilesView {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(AppLifecycleEvent event);

    // Module-only rendering surface. These methods keep all LVGL ownership in
    // the view while allowing controller state and intents to remain
    // framework-independent.
    static lv_obj_t* CreateWidgets();
    static void OnUnloadWidgets();
    static void RenderState(const files::ViewState& state);
    static bool ShowImagePreview(const char* posix_path);
    static void ClosePreviewVisual();
    static void ScheduleUsbStateRefresh();

    // UI-thread automation entry points. They apply the same SD/USB ownership
    // gates as tap handlers, then refresh the visible file browser.
    static bool PreviewPath(const char* posix_path);
    static bool PreviewPath(const char* posix_path,
                            const files_io_worker::TicketPtr& ticket);
    static bool IsPreviewFor(const char* posix_path);
    static void ApplyPendingIoResults();
    static bool RequestDeletePath(
        const char* posix_path, const files_io_worker::TicketPtr& ticket);
    static bool DeletePath(const char* posix_path);
    static bool GetStorageBytes(uint64_t* total_bytes, uint64_t* free_bytes);
};

}  // namespace agent_ui
