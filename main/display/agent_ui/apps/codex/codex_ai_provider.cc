#include "codex_ai_provider.h"

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
    if (!root) return;
    const std::string type = String(root.get(), "type");
    const std::string request = String(root.get(), "request_id").empty()
        ? String(root.get(), "requestId") : String(root.get(), "request_id");
    if (request.empty()) return;
    auto& store = Get();
    std::lock_guard<std::mutex> lock(store.mutex);
    auto found = store.pending.find(request);
    if (found == store.pending.end() || found->second.result.status != ai::OperationStatus::Pending) return;
    const auto& action = found->second.action;
    bool success = Bool(root.get(), "success");
    if (action == "realtime_start" || action == "realtime_end") {
        if (type != "realtime_status") return;
        const auto state = String(root.get(), "state");
        if (action == "realtime_start" && state == "listening" && Bool(root.get(), "acceptsAudio")) success = true;
        else if (state == "ended" || state == "error" || state == "disconnected") success = action == "realtime_end" && state == "ended";
        else return;
    } else if (action == "text") {
        if (type != "codex_text_result") return;
    } else if (action == "stop") {
        if (type != "turn_stop_result") return;
    } else if (action == "interaction") {
        if (type != "codex_interaction_result") return;
    } else {
        if (type != "codex_action_result" || String(root.get(), "action") != WireAction(action)) return;
    }
    auto& result = found->second.result;
    result.status = success ? ai::OperationStatus::Succeeded : ai::OperationStatus::Failed;
    result.result_json = json;
    if (!success) {
        result.error = String(root.get(), "error");
        if (result.error.empty()) result.error = String(root.get(), "message");
        if (result.error.empty()) result.error = "Codex request failed";
    }
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
