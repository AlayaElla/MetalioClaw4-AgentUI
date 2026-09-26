#pragma once

#include <cstdint>

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
    const ViewState& state() const { return controller_.state(); }
    uint64_t revision() const { return revision_; }
    uint64_t session() const { return session_; }
    bool SubmitIntent(const Intent& intent);

private:
    static void ApplyEvent(void* data);
    void HandleCommand(const Command& command);
    void PostEvent(const Event& event);

    Controller controller_;
    Adapter& adapter_;
    View view_;
    uint64_t revision_ = 0;
    uint64_t session_ = 0;
};

}  // namespace agent_ui::bluetooth
