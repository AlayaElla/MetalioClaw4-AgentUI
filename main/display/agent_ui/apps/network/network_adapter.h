#pragma once

#include <atomic>
#include <functional>

#include "network_contract.h"

namespace agent_ui::network {

class Adapter {
public:
    using EventSink = std::function<void(const Event&)>;
    using ConfigChangedSink = std::function<void()>;

    Adapter();
    ~Adapter();

    void SetEventSink(EventSink sink);
    void SetConfigChangedSink(ConfigChangedSink sink);
    void Execute(const Command& command);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace agent_ui::network
