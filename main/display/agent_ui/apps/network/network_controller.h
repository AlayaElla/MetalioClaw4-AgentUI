#pragma once

#include <functional>

#include "network_contract.h"
#include "network_view_state.h"

namespace agent_ui::network {

class Controller {
public:
    using StateSink = std::function<void(const ViewState&)>;
    using CommandSink = std::function<void(const Command&)>;

    void Activate(StateSink state_sink, CommandSink command_sink);
    void Deactivate();

    void HandleIntent(const Intent& intent);
    void HandleEvent(const Event& event);
    void HandleLifecycle(Lifecycle lifecycle);

    const ViewState& state() const { return state_; }

private:
    void PublishState() const;
    void Dispatch(const Command& command) const;

    ViewState state_;
    StateSink state_sink_;
    CommandSink command_sink_;
};

}  // namespace agent_ui::network
