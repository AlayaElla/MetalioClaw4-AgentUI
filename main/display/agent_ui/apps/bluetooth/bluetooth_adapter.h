#pragma once

#include <functional>

#include "bluetooth_contract.h"

namespace agent_ui::bluetooth {

class Adapter {
public:
    using EventSink = std::function<void(const Event&)>;

    static Adapter& Get();

    void Initialize();
    void SetEventSink(EventSink sink);
    void Execute(const Command& command);
    bool IsEnabled() const;
    bool IsConnected() const;

private:
    Adapter();
    ~Adapter();

    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace agent_ui::bluetooth
