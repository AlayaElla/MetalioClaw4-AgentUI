#pragma once

#include "bluetooth_adapter.h"
#include "bluetooth_controller.h"
#include "bluetooth_view.h"
#include "core/ui_utils.h"

namespace agent_ui::bluetooth {

class Module final {
public:
    Module();

    static void InitializeHardware();

    void BuildInto(lv_obj_t* parent);
    void ResetUi();
    void LifecycleCallback(AppLifecycleEvent event);

private:
    static void ApplyEvent(void* data);
    void HandleCommand(const Command& command);
    void PostEvent(const Event& event);

    Controller controller_;
    Adapter& adapter_;
    View view_;
};

}  // namespace agent_ui::bluetooth
