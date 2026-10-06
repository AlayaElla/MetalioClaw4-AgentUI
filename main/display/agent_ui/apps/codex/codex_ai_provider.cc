#include "codex_ai_provider.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <cJSON.h>

#include "ai/ai_availability.h"
#include "ai/ai_ui_operation.h"
#include "agent_ui/core/navigation.h"

namespace agent_ui::codex_ai {
namespace {
struct Pending {
    std::string action;
    ai::OperationResult result;
};
struct Store {
    std::mutex mutex;
    Hooks hooks;
    std::map<std::string, Pending> pending;
    uint64_t next = 0;
    bool registered = false;
};
Store& Get() { static Store value; return value; }
std::string String(const cJSON* root, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}
const char* Text(const cJSON* root, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : nullptr;
}
bool IsText(const cJSON* root, const char* key, const char* expected) {
    const char* value = Text(root, key);
    return value && expected && std::strcmp(value, expected) == 0;
}
bool Bool(const cJSON* root, const char* key);
size_t BoundedLength(const char* value, size_t limit) {
    size_t length = 0;
    while (length < limit && value[length] != '\0') ++length;
    return length;
}
std::string BoundedText(const cJSON* root, const char* key, size_t maximum) {
    const char* value = Text(root, key);
    if (!value) return {};
    const size_t length = BoundedLength(value, maximum);
    return std::string(value, length);
}
void AddBoundedText(cJSON* target, const cJSON* source, const char* key, size_t maximum) {
    const char* value = Text(source, key);
    if (!value) return;
    const size_t length = BoundedLength(value, maximum + 1);
    if (length <= maximum) cJSON_AddStringToObject(target, key, value);
}

constexpr size_t kMaximumAcknowledgementBytes = 2048;

std::string EscapeJsonText(const char* text, size_t maximum) {
    std::string escaped;
    if (!text) return escaped;
    const size_t length = BoundedLength(text, maximum);
    escaped.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += hex[c >> 4];
                    escaped += hex[c & 0x0f];
                } else escaped += static_cast<char>(c);
        }
    }
    return escaped;
}

std::string MinimalAcknowledgement(const char* type, const char* correlation_key, const char* request) {
    const std::string escaped_type = EscapeJsonText(type, 32);
    const std::string escaped_request = EscapeJsonText(request, 128);
    std::string output = "{\"type\":\"" + escaped_type + "\"";
    if (correlation_key && request) {
        output += ",\"";
        output += correlation_key;
        output += "\":\"" + escaped_request + "\"";
    }
    output += "}";
    return output;
}

bool AckFits(cJSON* object) {
    char* printed = cJSON_PrintUnformatted(object);
    if (!printed) return false;
    const size_t length = std::strlen(printed);
    cJSON_free(printed);
    return length <= kMaximumAcknowledgementBytes;
}

void AddOptionalAckText(cJSON* target, const cJSON* source, const char* key, size_t maximum) {
    AddBoundedText(target, source, key, maximum);
    if (!AckFits(target)) cJSON_DeleteItemFromObjectCaseSensitive(target, key);
}

std::string SerializeAcknowledgement(cJSON* result, const char* type,
                                     const char* correlation_key, const char* request) {
    char* printed = cJSON_PrintUnformatted(result);
    if (!printed) return MinimalAcknowledgement(type, correlation_key, request);
    const size_t length = std::strlen(printed);
    if (length > kMaximumAcknowledgementBytes) {
        cJSON_free(printed);
        return MinimalAcknowledgement(type, correlation_key, request);
    }
    std::string output(printed, length);
    cJSON_free(printed);
    return output;
}

std::string Acknowledgement(const cJSON* root, const char* type,
                            const char* correlation_key, const char* request) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(cJSON_CreateObject(), cJSON_Delete);
    if (!result) return MinimalAcknowledgement(type, correlation_key, request);
    const std::string bounded_type = type ? std::string(type, BoundedLength(type, 32)) : std::string();
    if (!cJSON_AddStringToObject(result.get(), "type", bounded_type.c_str()))
        return MinimalAcknowledgement(type, correlation_key, request);
    if (correlation_key && request &&
        !cJSON_AddStringToObject(result.get(), correlation_key, request))
        return MinimalAcknowledgement(type, correlation_key, request);
    AddOptionalAckText(result.get(), root, "action", 32);
    if (cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "success")))
        cJSON_AddBoolToObject(result.get(), "success", Bool(root, "success"));
    for (const char* field : {"host_id", "thread_id", "stream_id", "draft_id", "id", "status", "delivery", "outcome"})
        AddOptionalAckText(result.get(), root, field, 128);
    AddOptionalAckText(result.get(), root, "state", 32);
    AddOptionalAckText(result.get(), root, "error", 512);
    AddOptionalAckText(result.get(), root, "message", 512);
    if (cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "acceptsAudio")))
        cJSON_AddBoolToObject(result.get(), "acceptsAudio", Bool(root, "acceptsAudio"));
    if (cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "submissionConfirmed")))
        cJSON_AddBoolToObject(result.get(), "submissionConfirmed", Bool(root, "submissionConfirmed"));
    return SerializeAcknowledgement(result.get(), type, correlation_key, request);
}
bool Bool(const cJSON* root, const char* key) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, key));
}
ai::OperationResult Fail(const std::string& error) {
    ai::OperationResult result; result.error = error; return result;
}
ai::OperationResult Waiting() {
    ai::OperationResult result; result.status = ai::OperationStatus::Pending; return result;
}
const char* WireAction(const std::string& action) {
    if (action == "new") return "new_task";
    if (action == "select") return "select_task";
    if (action == "model") return "set_model";
    if (action == "effort") return "set_effort";
    if (action == "fast") return "set_fast";
    return "";
}
void Forget(const std::string& id) {
    std::lock_guard<std::mutex> lock(Get().mutex);
    Get().pending.erase(id);
}
}  // namespace

bool Register(const Hooks& hooks, std::string* error) {
    if (!hooks.state || !hooks.dispatch) {
        if (error) *error = "Codex provider hooks missing";
        return false;
    }
    auto& store = Get();
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        if (store.registered) return true;
        store.hooks = hooks;
    }
    ai::CapabilityProvider provider;
    provider.descriptor = {"codex.control", "Codex 控制",
        "Codex 任务、设置、消息与语音控制；操作前先打开页面并同步状态。",
        R"({"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["state","list","select","new","model","effort","fast","stop","realtime_start","realtime_end","preferences","reconnect","interaction","text"]},"slot":{"type":"integer"},"model":{"type":"string"},"effort":{"type":"string"},"fast":{"type":"boolean"},"text":{"type":"string"},"host_id":{"type":"string"},"thread_id":{"type":"string"},"draft_id":{"type":"string"},"stream_id":{"type":"string"},"interaction_id":{"type":"string"},"decision":{"type":"string"},"answers":{"type":"object"},"captions":{"type":"boolean"}}})"};
    // Discovery runs on the MCP thread. Mutable view state is read only in
    // UiOperations; an unopened view is an activation step, not unavailability.
    provider.get_state = [] {
        ai::CapabilityState state;
        state.state_json = "{\"opensOnInvoke\":true}";
        return state;
    };
    provider.get_result = ai::UiOperations::GetResult;
    provider.cancel = ai::UiOperations::Cancel;
    provider.invoke = [](const ai::InvokeRequest& request) {
        cJSON* root = cJSON_Parse(request.arguments_json.c_str());
        const std::string action = String(root, "action");
        cJSON_Delete(root);
        if (action.empty()) return Fail("Codex action is required");
        std::string id;
        {
            std::lock_guard<std::mutex> lock(Get().mutex);
            id = "codex-ai-" + std::to_string(++Get().next);
        }
        struct Invocation { bool sent = false; bool reconnecting = false; };
        auto invocation = std::make_shared<Invocation>();
        return ai::UiOperations::Submit(request, [request, action, id, invocation] {
            auto& store = Get();
            Hooks hooks;
            {
                std::lock_guard<std::mutex> lock(store.mutex);
                hooks = store.hooks;
            }
            if (!invocation->sent) {
                const auto policy = ai::Availability::Get().GetSnapshot();
                if (!policy.available || policy.generation != request.generation)
                    return Fail("AI availability changed before Codex dispatch");
                if (Navigation::Get().current() != ScreenId::Codex) {
                    Navigation::Get().Open(ScreenId::Codex);
                    return Waiting();
                }
                const auto state = hooks.state();
                if (action != "state" && action != "reconnect" && !state.available) return Waiting();
                if (action != "state" && action != "reconnect" && !state.enabled)
                    return Fail(state.reason.empty() ? "Codex is busy" : state.reason);
                {
                    std::lock_guard<std::mutex> lock(store.mutex);
                    if (store.pending.size() >= 16) return Fail("Too many Codex operations");
                    store.pending[id] = {action, Waiting()};
                }
                DispatchResult dispatched;
                try { dispatched = hooks.dispatch(action, request.arguments_json, id); }
                catch (...) { Forget(id); throw; }
                if (!dispatched.accepted) {
                    Forget(id);
                    return Fail(dispatched.error.empty() ? "Codex operation rejected" : dispatched.error);
                }
                invocation->sent = true;
                if (action == "reconnect") {
                    Forget(id);
                    invocation->reconnecting = true;
                    return Waiting();
                }
                if (dispatched.immediate) {
                    Forget(id);
                    ai::OperationResult done;
                    done.status = ai::OperationStatus::Succeeded;
                    done.result_json = dispatched.result_json;
                    return done;
                }
                if (!dispatched.correlation_id.empty() && dispatched.correlation_id != id) {
                    Forget(id);
                    if (hooks.cancel) hooks.cancel(dispatched.correlation_id);
                    return Fail("Codex transport returned a different request identity");
                }
            }
            if (Navigation::Get().current() != ScreenId::Codex) {
                Forget(id);
                if (hooks.cancel) hooks.cancel(id);
                return Fail("Codex page closed before the operation completed");
            }
            if (invocation->reconnecting) {
                if (!hooks.state().available) return Waiting();
                ai::OperationResult result;
                result.status = ai::OperationStatus::Succeeded;
                result.result_json = "{\"connected\":true}";
                return result;
            }
            std::lock_guard<std::mutex> lock(store.mutex);
            auto found = store.pending.find(id);
            if (found == store.pending.end()) return Fail("Codex acknowledgement was lost");
            auto result = found->second.result;
            if (result.status != ai::OperationStatus::Pending) store.pending.erase(found);
            return result;
        }, [id] {
            Hooks hooks;
            {
                std::lock_guard<std::mutex> lock(Get().mutex);
                hooks = Get().hooks;
                Get().pending.erase(id);
            }
            if (hooks.cancel) hooks.cancel(id);
        });
    };
    const bool registered = ai::CapabilityRegistry::Get().Register(std::move(provider), error);
    if (registered) {
        std::lock_guard<std::mutex> lock(store.mutex);
        store.registered = true;
    }
    return registered;
}

void ObserveMessage(const std::string& json) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json.c_str()), cJSON_Delete);
    ObserveMessage(root.get());
}

void ObserveMessage(const cJSON* root) {
    if (!root) return;
    const char* type = Text(root, "type");
    const char* request = Text(root, "request_id");
    const char* correlation_key = "request_id";
    if (!request || !*request) {
        request = Text(root, "requestId");
        correlation_key = "requestId";
    }
    if (!request || !*request) return;
    const size_t request_size = BoundedLength(request, 129);
    if (request_size == 0 || request_size > 128) return;
    const std::string request_key(request, request_size);
    auto& store = Get();
    std::lock_guard<std::mutex> lock(store.mutex);
    auto found = store.pending.find(request_key);
    if (found == store.pending.end() || found->second.result.status != ai::OperationStatus::Pending) return;
    const auto& action = found->second.action;
    bool success = Bool(root, "success");
    if (action == "realtime_start" || action == "realtime_end") {
        if (!IsText(root, "type", "realtime_status")) return;
        const char* state = Text(root, "state");
        if (!state) return;
        if (action == "realtime_start" && std::strcmp(state, "listening") == 0 && Bool(root, "acceptsAudio")) success = true;
        else if (std::strcmp(state, "ended") == 0 || std::strcmp(state, "error") == 0 || std::strcmp(state, "disconnected") == 0)
            success = action == "realtime_end" && std::strcmp(state, "ended") == 0;
        else return;
    } else if (action == "text") {
        if (!IsText(root, "type", "codex_text_result")) return;
    } else if (action == "stop") {
        if (!IsText(root, "type", "turn_stop_result")) return;
    } else if (action == "interaction") {
        if (!IsText(root, "type", "codex_interaction_result")) return;
    } else {
        if (!IsText(root, "type", "codex_action_result") || !IsText(root, "action", WireAction(action))) return;
    }
    auto& result = found->second.result;
    result.status = success ? ai::OperationStatus::Succeeded : ai::OperationStatus::Failed;
    result.result_json = Acknowledgement(root, type, correlation_key, request);
    if (!success) {
        result.error = BoundedText(root, "error", 512);
        if (result.error.empty()) result.error = BoundedText(root, "message", 512);
        if (result.error.empty()) result.error = "Codex request failed";
    }
}

void FailPendingRequest(const std::string& request_id, const std::string& error) {
    if (request_id.empty()) return;
    auto& store = Get();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto found = store.pending.find(request_id);
    if (found == store.pending.end() || found->second.result.status != ai::OperationStatus::Pending) return;
    found->second.result.status = ai::OperationStatus::Failed;
    const size_t length = BoundedLength(error.c_str(), 513);
    found->second.result.error = length <= 512 ? error : error.substr(0, 512);
    if (found->second.result.error.empty()) found->second.result.error = "Codex request could not be sent";
}

void CompletePendingRequest(const std::string& request_id, const std::string& result_json) {
    if (request_id.empty()) return;
    auto& store = Get();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto found = store.pending.find(request_id);
    if (found == store.pending.end() || found->second.result.status != ai::OperationStatus::Pending) return;
    found->second.result.status = ai::OperationStatus::Succeeded;
    const size_t length = BoundedLength(result_json.c_str(), 1025);
    found->second.result.result_json = length <= 1024 ? result_json : "{}";
    found->second.result.error.clear();
}

void Invalidate(const std::string& reason) {
    std::lock_guard<std::mutex> lock(Get().mutex);
    for (auto& entry : Get().pending) {
        if (entry.second.result.status != ai::OperationStatus::Pending) continue;
        entry.second.result.status = ai::OperationStatus::Cancelled;
        entry.second.result.error = reason;
    }
}
}  // namespace agent_ui::codex_ai
