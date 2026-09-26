#pragma once

#include <cstdint>

#include "lvgl.h"
#include "core/ui_utils.h"

namespace agent_ui {

// FilesView owns the SD browser, previews, and delete actions. SD card
// lifecycle remains with SdCardManager; this view only reads its state and
// renders the current directory.
class FilesView {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(AppLifecycleEvent event);

    // UI-thread automation entry points. They apply the same SD/USB ownership
    // gates as tap handlers, then refresh the visible file browser.
    static bool PreviewPath(const char* posix_path);
    static bool DeletePath(const char* posix_path);
    static bool GetStorageBytes(uint64_t* total_bytes, uint64_t* free_bytes);
};

}  // namespace agent_ui
