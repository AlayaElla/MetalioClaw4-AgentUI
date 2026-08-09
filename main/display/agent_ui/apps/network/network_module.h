#pragma once

#include "network_adapter.h"
#include "network_controller.h"
#include "network_view.h"

namespace agent_ui::network {

class Module final {
public:
    Module();

    void BuildInto(lv_obj_t* parent);
    void ResetUi();
    void LifecycleCallback(AppLifecycleEvent event);

    const ViewState& state() const { return controller_.state(); }

private:
    static void ApplyEvent(void* data);
    void HandleCommand(const Command& command);
    void PostEvent(const Event& event);

    Controller controller_;
    Adapter adapter_;
    View view_;
};

}  // namespace agent_ui::network
