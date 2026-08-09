#pragma once

#include <functional>

#include "camera_contract.h"
#include "camera_view_state.h"
#include "core/ui_utils.h"

namespace agent_ui::camera {

class Controller {
public:
    using StateSink = std::function<void(const ViewState&)>;
    using CommandSink = std::function<void(const Command&)>;

    void Activate(StateSink state_sink, CommandSink command_sink);
    void Deactivate();

    void HandleLifecycle(AppLifecycleEvent event);
    void HandleIntent(const Intent& intent);
    void HandleEvent(const Event& event);

    const ViewState& state() const { return state_; }
    uint32_t generation() const { return state_.generation; }

private:
    void Dispatch(Command command) const;
    void PublishState() const;
    void ResetForUnload();

    ViewState state_;
    StateSink state_sink_;
    CommandSink command_sink_;
};

}  // namespace agent_ui::camera
