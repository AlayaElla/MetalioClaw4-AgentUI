#include "ai_capabilities.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <exception>
#include <utility>

#include <cJSON.h>

#include "ai_availability.h"

namespace ai {
namespace {

constexpr size_t kMaxCapabilities = 48;
constexpr size_t kMaxPendingOperations = 16;
constexpr size_t kMaxOperationReceipts = 64;
constexpr size_t kMaxJsonBytes = 4096;
constexpr size_t kMaxJsonDepth = 16;

struct StoredOperation {
    std::string capability_id;
    std::string request_id;
    std::string arguments_json;
    uint32_t deadline_ms = 0;
    std::string provider_operation_id;
    OperationResult result;
    std::function<OperationResult(const std::string&)> get_result;
    std::function<bool(const std::string&)> cancel;
};

struct RegistryState {
    mutable std::mutex mutex;
    std::map<std::string, CapabilityProvider> providers;
    std::map<std::string, StoredOperation> operations;
    std::map<std::string, std::string> request_operations;
    uint64_t next_operation = 1;
    size_t pending_count = 0;
};

RegistryState& State() {
    static RegistryState state;
    return state;
}

OperationResult Failed(const std::string& error, uint64_t generation = 0) {
    OperationResult result;
    result.status = OperationStatus::Failed;
    result.generation = generation;
    result.error = error;
    return result;
}

bool IsTerminal(OperationStatus status) {
    return status != OperationStatus::Pending;
}

bool JsonDepthWithinLimit(const std::string& json) {
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (unsigned char character : json) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') in_string = false;
            continue;
        }
        if (character == '"') { in_string = true; continue; }
        if (character == '{' || character == '[') {
            if (++depth > kMaxJsonDepth) return false;
        } else if (character == '}' || character == ']') {
            if (depth == 0) return false;
            --depth;
        }
    }
    return !in_string && !escaped && depth == 0;
}

bool IsObjectJson(const std::string& json) {
    if (json.empty() || json.size() > kMaxJsonBytes || !JsonDepthWithinLimit(json)) return false;
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(json.c_str(), json.size(), &end, 0);
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    while (end != nullptr && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    const bool valid = end != nullptr && *end == '\0';
    cJSON_Delete(root);
    return valid;
}

bool JsonMatchesSchema(const cJSON* value, const cJSON* schema, std::string* error) {
    if (schema == nullptr || !cJSON_IsObject(schema)) return true;
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (cJSON_IsString(type) && type->valuestring != nullptr) {
        const std::string wanted = type->valuestring;
        const bool matches = wanted == "object" ? cJSON_IsObject(value) :
                             wanted == "string" ? cJSON_IsString(value) :
                             wanted == "boolean" ? cJSON_IsBool(value) :
                             wanted == "integer" ? (cJSON_IsNumber(value) && value->valuedouble == value->valueint) :
                             wanted == "number" ? cJSON_IsNumber(value) : true;
        if (!matches) { if (error) *error = "arguments do not match schema type"; return false; }
    }
    const cJSON* values = cJSON_GetObjectItemCaseSensitive(schema, "enum");
    if (cJSON_IsArray(values)) {
        bool matched = false;
        cJSON* option = nullptr;
        cJSON_ArrayForEach(option, values) {
            if ((cJSON_IsString(value) && cJSON_IsString(option) &&
                 std::string(value->valuestring ? value->valuestring : "") ==
                     std::string(option->valuestring ? option->valuestring : "")) ||
                (cJSON_IsNumber(value) && cJSON_IsNumber(option) &&
                 value->valuedouble == option->valuedouble) ||
                (cJSON_IsBool(value) && cJSON_IsBool(option) &&
                 cJSON_IsTrue(value) == cJSON_IsTrue(option))) { matched = true; break; }
        }
        if (!matched) { if (error) *error = "argument is outside schema enum"; return false; }
    }
    if (cJSON_IsNumber(value)) {
        const cJSON* minimum = cJSON_GetObjectItemCaseSensitive(schema, "minimum");
        const cJSON* maximum = cJSON_GetObjectItemCaseSensitive(schema, "maximum");
        if ((cJSON_IsNumber(minimum) && value->valuedouble < minimum->valuedouble) ||
            (cJSON_IsNumber(maximum) && value->valuedouble > maximum->valuedouble)) {
            if (error) *error = "number is outside schema range";
            return false;
        }
    }
    if (cJSON_IsString(value) && value->valuestring != nullptr) {
        const size_t length = std::strlen(value->valuestring);
        const cJSON* min_length = cJSON_GetObjectItemCaseSensitive(schema, "minLength");
        const cJSON* max_length = cJSON_GetObjectItemCaseSensitive(schema, "maxLength");
        if ((cJSON_IsNumber(min_length) && length < static_cast<size_t>(min_length->valueint)) ||
            (cJSON_IsNumber(max_length) && length > static_cast<size_t>(max_length->valueint))) {
            if (error) *error = "string is outside schema length";
            return false;
        }
    }
    if (!cJSON_IsObject(value)) return true;
    const cJSON* required = cJSON_GetObjectItemCaseSensitive(schema, "required");
    if (cJSON_IsArray(required)) {
        cJSON* key = nullptr;
        cJSON_ArrayForEach(key, required) {
            if (!cJSON_IsString(key) || key->valuestring == nullptr ||
                cJSON_GetObjectItemCaseSensitive(value, key->valuestring) == nullptr) {
                if (error) *error = "required schema argument is missing";
                return false;
            }
        }
    }
    const cJSON* properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (cJSON_IsObject(properties)) {
        cJSON* property = nullptr;
        cJSON_ArrayForEach(property, properties) {
            const cJSON* argument = cJSON_GetObjectItemCaseSensitive(value, property->string);
            if (argument != nullptr && !JsonMatchesSchema(argument, property, error)) return false;
        }
    }
    const cJSON* additional = cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties");
    if (cJSON_IsFalse(additional)) {
        cJSON* argument = nullptr;
        cJSON_ArrayForEach(argument, value) {
            if (!cJSON_IsObject(properties) ||
                cJSON_GetObjectItemCaseSensitive(properties, argument->string) == nullptr) {
                if (error) *error = "unexpected schema argument";
                return false;
            }
        }
    }
    return true;
}

bool ValidateSchema(const std::string& arguments, const std::string& schema_json,
                    std::string* error) {
    cJSON* arguments_root = cJSON_ParseWithLength(arguments.c_str(), arguments.size());
    cJSON* schema_root = cJSON_ParseWithLength(schema_json.c_str(), schema_json.size());
    const bool valid = arguments_root != nullptr && schema_root != nullptr &&
                       JsonMatchesSchema(arguments_root, schema_root, error);
    cJSON_Delete(arguments_root);
    cJSON_Delete(schema_root);
    return valid;
}

bool IsValidId(const std::string& id) {
    if (id.empty() || id.size() > 80) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
               ch == '.' || ch == '_' || ch == '-';
    });
}

void RemoveRequestIndexLocked(RegistryState& state, const StoredOperation& operation) {
    if (!operation.request_id.empty()) {
        const auto found = state.request_operations.find(operation.request_id);
        if (found != state.request_operations.end() &&
            found->second == operation.result.operation_id) {
            state.request_operations.erase(found);
        }
    }
}

void TrimReceiptsLocked(RegistryState& state) {
    while (state.operations.size() >= kMaxOperationReceipts) {
        auto victim = std::find_if(state.operations.begin(), state.operations.end(),
                                   [](const auto& entry) {
            return IsTerminal(entry.second.result.status);
        });
        if (victim == state.operations.end()) return;
        RemoveRequestIndexLocked(state, victim->second);
        state.operations.erase(victim);
    }
}

std::string AllocateOperationIdLocked(RegistryState& state) {
    std::string id;
    do { id = "op-" + std::to_string(state.next_operation++); }
    while (state.operations.find(id) != state.operations.end());
    return id;
}

void SetTerminalLocked(RegistryState& state, StoredOperation& operation,
                       OperationResult result) {
    if (IsTerminal(operation.result.status)) return;
    result.operation_id = operation.result.operation_id;
    operation.result = std::move(result);
    operation.get_result = {};
    operation.cancel = {};
    if (state.pending_count > 0) --state.pending_count;
}

void RefreshPendingOperations() {
    struct Poll {
        std::string operation_id;
        std::string provider_operation_id;
        std::function<OperationResult(const std::string&)> get_result;
    };
    std::vector<Poll> polls;
    RegistryState& state = State();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        for (const auto& entry : state.operations) {
            const StoredOperation& operation = entry.second;
            if (operation.result.status == OperationStatus::Pending &&
                !operation.provider_operation_id.empty() && operation.get_result) {
                polls.push_back({entry.first, operation.provider_operation_id,
                                 operation.get_result});
            }
        }
    }
    for (const Poll& poll : polls) {
        OperationResult result;
        try {
            result = poll.get_result(poll.provider_operation_id);
        } catch (const std::exception& error) {
            result = Failed(error.what());
        } catch (...) {
            result = Failed("provider result callback failed");
        }
        if (result.status == OperationStatus::Pending) continue;
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto found = state.operations.find(poll.operation_id);
        if (found != state.operations.end() &&
            found->second.provider_operation_id == poll.provider_operation_id) {
            SetTerminalLocked(state, found->second, std::move(result));
        }
    }
}

}  // namespace

CapabilityRegistry& CapabilityRegistry::Get() {
    static CapabilityRegistry registry;
    return registry;
}

const char* OperationStatusName(OperationStatus status) {
    switch (status) {
        case OperationStatus::Pending: return "pending";
        case OperationStatus::Succeeded: return "succeeded";
        case OperationStatus::Failed: return "failed";
        case OperationStatus::Cancelled: return "cancelled";
    }
    return "failed";
}

bool CapabilityRegistry::Register(CapabilityProvider provider, std::string* error) {
    if (!IsValidId(provider.descriptor.id) || provider.descriptor.title.empty() ||
        !provider.get_state || !provider.invoke ||
        !IsObjectJson(provider.descriptor.args_schema_json)) {
        if (error != nullptr) *error = "invalid capability provider";
        return false;
    }
    RegistryState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.providers.find(provider.descriptor.id) != state.providers.end()) {
        if (error != nullptr) *error = "capability already registered";
        return false;
    }
    if (state.providers.size() >= kMaxCapabilities) {
        if (error != nullptr) *error = "capability registry is full";
        return false;
    }
    state.providers.emplace(provider.descriptor.id, std::move(provider));
    return true;
}

bool CapabilityRegistry::Unregister(const std::string& id, std::string* error) {
    std::vector<std::pair<std::string, std::function<bool(const std::string&)>>> cancels;
    {
        RegistryState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.providers.erase(id) == 0) return true;
        for (auto& entry : state.operations) {
            StoredOperation& operation = entry.second;
            if (operation.capability_id != id || IsTerminal(operation.result.status)) continue;
            if (operation.cancel && !operation.provider_operation_id.empty()) {
                cancels.emplace_back(operation.provider_operation_id, operation.cancel);
            }
            operation.result.status = OperationStatus::Cancelled;
            operation.result.error = "Capability provider was unregistered";
            operation.get_result = {};
            operation.cancel = {};
            if (state.pending_count > 0) --state.pending_count;
        }
    }
    for (const auto& cancel : cancels) {
        try { cancel.second(cancel.first); } catch (...) {}
    }
    return true;
}

std::vector<CapabilityDescriptor> CapabilityRegistry::List() const {
    RegistryState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    std::vector<CapabilityDescriptor> result;
    result.reserve(state.providers.size());
    for (const auto& entry : state.providers) result.push_back(entry.second.descriptor);
    return result;
}

bool CapabilityRegistry::Describe(const std::string& id, CapabilityDescriptor* descriptor,
                                  CapabilityState* capability_state, std::string* error) const {
    CapabilityProvider provider;
    {
        RegistryState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto found = state.providers.find(id);
        if (found == state.providers.end()) {
            if (error != nullptr) *error = "unknown capability";
            return false;
        }
        provider = found->second;
    }
    if (descriptor != nullptr) *descriptor = provider.descriptor;
    if (capability_state != nullptr) {
        try {
            *capability_state = provider.get_state();
        } catch (const std::exception& exception) {
            capability_state->available = false;
            capability_state->enabled = false;
            capability_state->reason = exception.what();
        } catch (...) {
            capability_state->available = false;
            capability_state->enabled = false;
            capability_state->reason = "provider state callback failed";
        }
    }
    return true;
}

OperationResult CapabilityRegistry::Invoke(const InvokeRequest& request) {
    if (!IsValidId(request.capability_id) || !IsObjectJson(request.arguments_json)) {
        return Failed("invalid capability id or arguments JSON");
    }
    const auto availability = Availability::Get().GetSnapshot();
    if (!availability.available) return Failed("AI control is disabled", availability.generation);
    if (request.generation != 0 && request.generation != availability.generation) {
        return Failed("stale capability request", availability.generation);
    }

    RefreshPendingOperations();
    CapabilityProvider provider;
    std::string registry_operation_id;
    {
        RegistryState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!request.request_id.empty()) {
            const auto indexed = state.request_operations.find(request.request_id);
            if (indexed != state.request_operations.end()) {
                const auto existing = state.operations.find(indexed->second);
                if (existing != state.operations.end()) {
                    const StoredOperation& operation = existing->second;
                    if (operation.capability_id != request.capability_id ||
                        operation.arguments_json != request.arguments_json ||
                        operation.deadline_ms != request.deadline_ms) {
                        return Failed("request_id conflicts with different capability arguments", availability.generation);
                    }
                    return operation.result;
                }
                state.request_operations.erase(indexed);
            }
        }
        const auto found = state.providers.find(request.capability_id);
        if (found == state.providers.end()) return Failed("unknown capability", availability.generation);
        std::string schema_error;
        if (!ValidateSchema(request.arguments_json, found->second.descriptor.args_schema_json,
                            &schema_error)) {
            return Failed(schema_error.empty() ? "arguments do not match capability schema" :
                                              schema_error,
                          availability.generation);
        }
        TrimReceiptsLocked(state);
        if (state.operations.size() >= kMaxOperationReceipts) {
            return Failed("operation receipt store is full", availability.generation);
        }
        if (state.pending_count >= kMaxPendingOperations) {
            return Failed("too many pending operations", availability.generation);
        }
        // Insert an in-flight receipt before provider code runs. This makes a
        // request id idempotent even when two callers arrive concurrently.
        ++state.pending_count;
        provider = found->second;
        registry_operation_id = AllocateOperationIdLocked(state);
        StoredOperation inflight;
        inflight.capability_id = request.capability_id;
        inflight.request_id = request.request_id;
        inflight.arguments_json = request.arguments_json;
        inflight.deadline_ms = request.deadline_ms;
        inflight.result.operation_id = registry_operation_id;
        inflight.result.status = OperationStatus::Pending;
        inflight.result.generation = availability.generation;
        inflight.get_result = provider.get_result;
        inflight.cancel = provider.cancel;
        state.operations.emplace(registry_operation_id, std::move(inflight));
        if (!request.request_id.empty()) {
            state.request_operations[request.request_id] = registry_operation_id;
        }
    }

    CapabilityState capability_state;
    try {
        capability_state = provider.get_state();
    } catch (const std::exception& error) {
        capability_state.available = false;
        capability_state.reason = error.what();
    } catch (...) {
        capability_state.available = false;
        capability_state.reason = "provider state callback failed";
    }
    if (!capability_state.available || !capability_state.enabled) {
        RegistryState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto found = state.operations.find(registry_operation_id);
        if (found == state.operations.end()) return Failed("capability was unregistered", availability.generation);
        SetTerminalLocked(state, found->second,
                          Failed(capability_state.reason.empty() ? "capability unavailable" : capability_state.reason,
                                 availability.generation));
        return found->second.result;
    }

    InvokeRequest checked = request;
    checked.generation = availability.generation;
    OperationResult result;
    try {
        result = provider.invoke(checked);
    } catch (const std::exception& error) {
        result = Failed(error.what(), availability.generation);
    } catch (...) {
        result = Failed("provider invoke callback failed", availability.generation);
    }
    result.generation = availability.generation;
    if (result.status == OperationStatus::Pending && (!provider.get_result || !provider.cancel)) {
        result = Failed("pending operation has no result/cancel handler", availability.generation);
    }

    RegistryState& state = State();
    std::function<bool(const std::string&)> cancel_after_unregister;
    std::string provider_operation_id;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto found = state.operations.find(registry_operation_id);
        if (found == state.operations.end()) return Failed("capability was unregistered", availability.generation);
        StoredOperation& stored = found->second;
        provider_operation_id = result.operation_id;
        if (IsTerminal(stored.result.status)) {
            // Unregister/cancel won the race. Never resurrect the receipt.
            cancel_after_unregister = provider.cancel;
        } else if (result.status == OperationStatus::Pending) {
            if (provider_operation_id.empty()) {
                SetTerminalLocked(state, stored,
                                  Failed("pending provider returned no operation_id", availability.generation));
            } else {
                stored.provider_operation_id = provider_operation_id;
                stored.get_result = provider.get_result;
                stored.cancel = provider.cancel;
            }
        } else {
            SetTerminalLocked(state, stored, std::move(result));
        }
        if (!IsTerminal(stored.result.status)) return stored.result;
        result = stored.result;
    }
    if (cancel_after_unregister && !provider_operation_id.empty()) {
        try { cancel_after_unregister(provider_operation_id); } catch (...) {}
    }
    return result;
}

OperationResult CapabilityRegistry::GetResult(const std::string& operation_id) const {
    StoredOperation operation;
    {
        RegistryState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto found = state.operations.find(operation_id);
        if (found == state.operations.end()) return Failed("unknown operation");
        if (IsTerminal(found->second.result.status)) return found->second.result;
        operation = found->second;
    }
    OperationResult result;
    try {
        result = operation.get_result(operation.provider_operation_id);
    } catch (const std::exception& error) {
        result = Failed(error.what());
    } catch (...) {
        result = Failed("provider result callback failed");
    }
    result.operation_id = operation_id;
    if (result.status == OperationStatus::Pending) return result;

    RegistryState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.operations.find(operation_id);
    if (found != state.operations.end() && !IsTerminal(found->second.result.status)) {
        SetTerminalLocked(state, found->second, std::move(result));
        return found->second.result;
    }
    return result;
}

bool CapabilityRegistry::Cancel(const std::string& operation_id, std::string* error) {
    std::function<bool(const std::string&)> cancel;
    std::string provider_operation_id;
    {
        RegistryState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto found = state.operations.find(operation_id);
        if (found == state.operations.end()) {
            if (error != nullptr) *error = "unknown operation";
            return false;
        }
        if (IsTerminal(found->second.result.status) || !found->second.cancel) {
            if (error != nullptr) *error = "operation cannot be cancelled";
            return false;
        }
        cancel = found->second.cancel;
        provider_operation_id = found->second.provider_operation_id;
        if (provider_operation_id.empty()) {
            SetTerminalLocked(state, found->second,
                              Failed("Operation cancelled before provider started"));
            found->second.result.status = OperationStatus::Cancelled;
            return true;
        }
    }
    bool cancelled = false;
    try { cancelled = cancel(provider_operation_id); } catch (...) { cancelled = false; }
    if (!cancelled) {
        if (error != nullptr) *error = "operation cannot be cancelled";
        return false;
    }
    RegistryState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.operations.find(operation_id);
    if (found != state.operations.end() && !IsTerminal(found->second.result.status)) {
        OperationResult cancelled_result;
        cancelled_result.status = OperationStatus::Cancelled;
        cancelled_result.error = "Operation cancelled; completed effects are not undone";
        SetTerminalLocked(state, found->second, std::move(cancelled_result));
    }
    return true;
}

void CapabilityRegistry::ResetForTest() {
    RegistryState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.providers.clear();
    state.operations.clear();
    state.request_operations.clear();
    state.next_operation = 1;
    state.pending_count = 0;
}

}  // namespace ai
