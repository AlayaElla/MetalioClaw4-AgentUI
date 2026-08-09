#pragma once

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
};

}  // namespace agent_ui
