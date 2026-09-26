#pragma once
#include "ai_capabilities.h"
#include <functional>

namespace ai {
// Reusable bounded UI-thread execution with completion polling. A step may
// dispatch on its first call and observe state on subsequent calls. Once a
// command has started, policy changes do not erase its real completion result.
class UiOperations {
public:
    using Step = std::function<OperationResult()>;
    static OperationResult Submit(const InvokeRequest& request, Step step,
                                  std::function<void()> cancel = {});
    static OperationResult GetResult(const std::string& id);
    static bool Cancel(const std::string& id);
};
}  // namespace ai
