#pragma once

#include "camera_adapter.h"
#include "camera_controller.h"
#include "camera_view.h"

namespace agent_ui::camera {

class Module final {
public:
    Module();

    lv_obj_t* Mount();
    void ResetUi();
    void LifecycleCallback(AppLifecycleEvent event);

    const ViewState& state() const { return controller_.state(); }

    // Public automation seam. Calls must originate on the LVGL/UI thread;
    // providers use ai::UiOperations to marshal and observe them safely.
    static bool SubmitIntent(const Intent& intent);
    static const ViewState* ActiveState();
    static bool IsActive();

    static void LifecycleThunk(AppLifecycleEvent event);

private:
    static void ApplyEvent(void* data);
    void HandleCommand(const Command& command);
    void PostEvent(const Event& event);
    bool SubmitIntentOnUiThread(const Intent& intent);

    Controller controller_;
    Adapter adapter_;
    View view_;
};

}  // namespace agent_ui::camera
