#pragma once

#include "lvgl.h"

namespace agent_ui {

using SwipeBackCallback = void (*)();

enum class AppLifecycleEvent {
    Load,
    Unload,
    Suspend,
    Resume,
};

using AppLifecycleCallback = void (*)(AppLifecycleEvent event);

// Remove generic LVGL container chrome when an object is used for layout.
void StripObjectChrome(lv_obj_t* object);

// Let pointer input pass through an object and all of its descendants.
void MakeInputPassive(lv_obj_t* object);

// Exclude a control from the global swipe-back recognizer.
void IgnoreSwipeBack(lv_obj_t* object, bool recursive = true);

// Add a low-cost right-swipe gesture to an App root.
void AttachSwipeBack(lv_obj_t* root, SwipeBackCallback on_back);

// Bind foreground lifecycle events and the global idle activity notifier.
void AttachAppLifecycle(lv_obj_t* root, AppLifecycleCallback callback);

// Pause/resume an already-mounted app without unloading its LVGL tree. This
// lets standby stop app-owned workers while preserving the exact UI state.
void SendAppLifecycle(lv_obj_t* root, AppLifecycleEvent event);

}  // namespace agent_ui
