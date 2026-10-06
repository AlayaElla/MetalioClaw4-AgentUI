#pragma once

#include <cstdint>
#include <array>

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
    void SetConfigChangedCallback(Adapter::ConfigChangedSink callback);

    const ViewState& state() const { return controller_.state(); }
    uint64_t revision() const { return revision_; }
    // UI-thread automation seam; this dispatches through the same controller
    // used by touch input and never fabricates an adapter completion.
    bool SubmitIntent(const Intent& intent);
    bool SetMode(int mode);
    uint64_t session() const { return session_; }
    uint64_t EventRevision(EventType type) const { return event_revisions_[static_cast<size_t>(type)]; }
    bool scan_succeeded() const { return scan_succeeded_; }

private:
    static void ApplyEvent(void* data);
    void HandleCommand(const Command& command);
    void PostEvent(const Event& event);

    Controller controller_;
    Adapter adapter_;
    View view_;
    uint64_t revision_ = 0;
    uint64_t session_ = 0;
    std::array<uint64_t, static_cast<size_t>(EventType::SimSlotSynced) + 1> event_revisions_{};
    bool scan_succeeded_ = false;
};

}  // namespace agent_ui::network
