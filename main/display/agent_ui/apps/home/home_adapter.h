#pragma once

#include "home_contract.h"

namespace agent_ui::home {

class Adapter {
public:
    void Execute(const Command& command);
};

}  // namespace agent_ui::home
