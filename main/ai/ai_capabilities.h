#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ai {

// JSON is deliberately carried as text.  MCP's current property layer only
// exposes scalar fields, while capabilities need evolvable argument schemas.
struct CapabilityDescriptor {
    std::string id;
    std::string title;
    std::string description;
    std::string args_schema_json = "{\"type\":\"object\"}";
};

struct CapabilityState {
    bool available = true;
    bool enabled = true;
    uint64_t generation = 0;
    std::string state_json = "{}";
    std::string reason;
};

enum class OperationStatus : uint8_t { Pending, Succeeded, Failed, Cancelled };

struct OperationResult {
    std::string operation_id;
    OperationStatus status = OperationStatus::Failed;
    uint64_t generation = 0;
    std::string result_json = "{}";
    std::string error;
};

struct InvokeRequest {
    std::string capability_id;
    std::string arguments_json = "{}";
    std::string request_id;
    uint32_t deadline_ms = 0;
    uint64_t generation = 0;
};

// A provider may return Pending only when it also supplies a result and
// cancellation callback.  This keeps queued work observable and bounded.
struct CapabilityProvider {
    CapabilityDescriptor descriptor;
    std::function<CapabilityState()> get_state;
    std::function<OperationResult(const InvokeRequest&)> invoke;
    std::function<OperationResult(const std::string& operation_id)> get_result;
    std::function<bool(const std::string& operation_id)> cancel;
};

class CapabilityRegistry {
public:
    static CapabilityRegistry& Get();

    bool Register(CapabilityProvider provider, std::string* error = nullptr);
    // Removes a dynamic provider and cancels every operation it owns. Safe to
    // call repeatedly during an app/provider shutdown.
    bool Unregister(const std::string& id, std::string* error = nullptr);
    std::vector<CapabilityDescriptor> List() const;
    bool Describe(const std::string& id, CapabilityDescriptor* descriptor,
                  CapabilityState* state, std::string* error = nullptr) const;
    OperationResult Invoke(const InvokeRequest& request);
    OperationResult GetResult(const std::string& operation_id) const;
    bool Cancel(const std::string& operation_id, std::string* error = nullptr);

    // Test/controlled shutdown hook.  Production callers do not clear the
    // registry after Runtime::Initialize().
    void ResetForTest();

private:
    CapabilityRegistry() = default;
};

const char* OperationStatusName(OperationStatus status);

}  // namespace ai
