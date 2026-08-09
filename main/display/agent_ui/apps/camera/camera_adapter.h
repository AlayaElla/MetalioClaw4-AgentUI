#pragma once

#include <functional>
#include <memory>

#include "camera_contract.h"

namespace agent_ui::camera {

class Adapter {
public:
    using EventSink = std::function<void(const Event&)>;

    Adapter();
    ~Adapter();

    void SetEventSink(EventSink sink);
    void Execute(const Command& command);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace agent_ui::camera
