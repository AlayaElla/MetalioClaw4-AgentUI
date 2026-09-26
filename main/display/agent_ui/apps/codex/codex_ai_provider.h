#pragma once

#include <functional>
#include <string>

#include "ai/ai_capabilities.h"

namespace agent_ui::codex_ai {

struct DispatchResult {
    bool accepted = false;
    bool immediate = false;
    std::string correlation_id;
    std::string result_json = "{}";
    std::string error;
};

struct Hooks {
    std::function<ai::CapabilityState()> state;
    std::function<DispatchResult(const std::string& action, const std::string& arguments,
                                 const std::string& request_id)> dispatch;
    std::function<void(const std::string& correlation_id)> cancel;
};

// Registers the generic, foreground-only Codex control surface.  The view
// supplies real transport/UI operations; this class owns pending operation and
// acknowledgement correlation rather than treating a WebSocket write as success.
bool Register(const Hooks& hooks, std::string* error = nullptr);
void ObserveMessage(const std::string& json);
void Invalidate(const std::string& reason);

}  // namespace agent_ui::codex_ai
