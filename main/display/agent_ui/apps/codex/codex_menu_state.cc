#include "codex_menu_state.h"

#include <cstring>
#include <memory>

#include "cJSON.h"

namespace agent_ui::codex_menu {
namespace {
const char* String(const cJSON* value) { return cJSON_IsString(value) ? value->valuestring : ""; }
SlotState ParseSlotState(const char* value) {
    if (std::strcmp(value, "unbound") == 0) return SlotState::Unbound;
    if (std::strcmp(value, "idle") == 0) return SlotState::Idle;
    if (std::strcmp(value, "working") == 0) return SlotState::Working;
    if (std::strcmp(value, "waiting") == 0) return SlotState::Waiting;
    if (std::strcmp(value, "error") == 0) return SlotState::Error;
    return SlotState::Unknown;
}
std::string Escape(const std::string& value) {
    std::string out;
    for (char c : value) { if (c == '\\' || c == '"') out += '\\'; out += c; }
    return out;
}
void SetError(std::string* error, const char* text) { if (error) *error = text; }
std::string BoundedString(const cJSON* object, const char* key, size_t max_bytes) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || !value->valuestring) return {};
    size_t length = 0;
    while (length <= max_bytes && value->valuestring[length] != '\0') ++length;
    return length <= max_bytes ? std::string(value->valuestring, length) : std::string{};
}
std::vector<InteractionOption> ParseOptions(const cJSON* values, size_t maximum) {
    std::vector<InteractionOption> out;
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, values) {
        if (out.size() >= maximum) break;
        InteractionOption option{BoundedString(entry, "id", 192), BoundedString(entry, "label", 512), BoundedString(entry, "description", 2048)};
        if (!option.label.empty()) out.push_back(std::move(option));
    }
    return out;
}
void ParseSlot(const cJSON* entry, Slot* slot) {
    if (entry == nullptr || slot == nullptr || !cJSON_IsObject(entry)) return;
    slot->host_id = String(cJSON_GetObjectItemCaseSensitive(entry, "hostId"));
    slot->thread_id = String(cJSON_GetObjectItemCaseSensitive(entry, "threadId"));
    slot->title = String(cJSON_GetObjectItemCaseSensitive(entry, "title"));
    slot->model = String(cJSON_GetObjectItemCaseSensitive(entry, "model"));
    slot->effort = String(cJSON_GetObjectItemCaseSensitive(entry, "effort"));
    slot->service_tier = String(cJSON_GetObjectItemCaseSensitive(entry, "serviceTier"));
    slot->state = ParseSlotState(String(cJSON_GetObjectItemCaseSensitive(entry, "state")));
    slot->synced = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "synced"));
    const cJSON* model_ready = cJSON_GetObjectItemCaseSensitive(entry, "modelReady");
    const cJSON* effort_ready = cJSON_GetObjectItemCaseSensitive(entry, "effortReady");
    const cJSON* fast_ready = cJSON_GetObjectItemCaseSensitive(entry, "fastReady");
    slot->model_ready = cJSON_IsBool(model_ready) ? cJSON_IsTrue(model_ready) : slot->synced;
    slot->effort_ready = cJSON_IsBool(effort_ready) ? cJSON_IsTrue(effort_ready) : slot->synced;
    const cJSON* fast = cJSON_GetObjectItemCaseSensitive(entry, "fast");
    slot->has_fast = cJSON_IsBool(fast); slot->fast = cJSON_IsTrue(fast);
    slot->fast_ready = cJSON_IsBool(fast_ready) ? cJSON_IsTrue(fast_ready) : slot->has_fast;
    const cJSON* updated = cJSON_GetObjectItemCaseSensitive(entry, "updatedAt");
    slot->updated_at = cJSON_IsNumber(updated) ? static_cast<int64_t>(updated->valuedouble) : 0;
}
bool ParseRoot(const cJSON* root, State* out, std::string* error) {
    if (!root) { SetError(error, "invalid JSON"); return false; }
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || std::strcmp(type->valuestring, "codex_state") != 0) {
        SetError(error, "not codex_state"); return false;
    }
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(version) || version->valueint != 1) {
        SetError(error, "unsupported state version"); return false;
    }
    State parsed{};
    parsed.version = version->valueint;
    const cJSON* revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
    parsed.revision = cJSON_IsNumber(revision) ? static_cast<uint64_t>(revision->valuedouble) : 0;
    const cJSON* connected = cJSON_GetObjectItemCaseSensitive(root, "connected");
    parsed.connected = cJSON_IsTrue(connected);
    parsed.source = String(cJSON_GetObjectItemCaseSensitive(root, "source"));
    parsed.stream_id = String(cJSON_GetObjectItemCaseSensitive(root, "streamId"));
    parsed.draft_request_id = String(cJSON_GetObjectItemCaseSensitive(root, "draftRequestId"));
    parsed.draft_status = String(cJSON_GetObjectItemCaseSensitive(root, "draftStatus"));
    parsed.draft_binding = parsed.draft_status == "binding";
    parsed.draft_settings_loading = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "draftSettingsLoading"));
    parsed.draft_submitted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "draftSubmitted"));
    const cJSON* draft_settings = cJSON_GetObjectItemCaseSensitive(root, "draftSettings");
    if (!parsed.draft_request_id.empty()) {
        ParseSlot(draft_settings, &parsed.draft_settings);
        parsed.draft_settings.host_id = "local";
        parsed.draft_settings.thread_id.clear();
        parsed.draft_settings.title = "新任务";
        parsed.draft_settings_error = BoundedString(draft_settings, "error", 512);
        if (parsed.draft_status == "error") {
            parsed.draft_settings_error = BoundedString(root, "draftError", 512);
            if (parsed.draft_settings_error.empty()) parsed.draft_settings_error = "新任务准备失败，请重试";
        } else if (parsed.draft_binding) {
            parsed.draft_settings_error = BoundedString(root, "draftError", 512);
            if (parsed.draft_settings_error.empty()) parsed.draft_settings_error = "正在连接新任务…";
        }
    }
    const cJSON* capabilities = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
    if (cJSON_IsObject(capabilities)) {
        parsed.can_select_task = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(capabilities, "selectTask"));
        parsed.can_new_task = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(capabilities, "newTask"));
        parsed.can_set_model = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(capabilities, "setModel"));
        parsed.can_set_effort = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(capabilities, "setEffort"));
        parsed.can_set_fast = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(capabilities, "setFast"));
    }
    const cJSON* selected = cJSON_GetObjectItemCaseSensitive(root, "selectedSlot");
    parsed.selected_slot = cJSON_IsNumber(selected) && selected->valueint >= 0 && selected->valueint < 6 ? selected->valueint : -1;
    for (int i = 0; i < 6; ++i) parsed.slots[i].slot = i;
    const cJSON* slots = cJSON_GetObjectItemCaseSensitive(root, "slots");
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, slots) {
        const cJSON* index = cJSON_GetObjectItemCaseSensitive(entry, "slot");
        if (!cJSON_IsNumber(index) || index->valueint < 0 || index->valueint >= 6) continue;
        Slot& slot = parsed.slots[index->valueint];
        ParseSlot(entry, &slot);
    }
    ParseSlot(cJSON_GetObjectItemCaseSensitive(root, "activeTask"), &parsed.active_task);
    const cJSON* conversation = cJSON_GetObjectItemCaseSensitive(root, "conversation");
    const std::string host = String(cJSON_GetObjectItemCaseSensitive(conversation, "hostId"));
    const std::string thread = String(cJSON_GetObjectItemCaseSensitive(conversation, "threadId"));
    if (MatchesSelectedTask(parsed, host, thread)) {
        parsed.conversation.host_id = host;
        parsed.conversation.thread_id = thread;
        parsed.conversation.ready = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(conversation, "ready"));
        if (parsed.conversation.ready) {
            const cJSON* messages = cJSON_GetObjectItemCaseSensitive(conversation, "messages");
            cJSON* message = nullptr;
            cJSON_ArrayForEach(message, messages) {
                Message value{String(cJSON_GetObjectItemCaseSensitive(message, "id")),
                              String(cJSON_GetObjectItemCaseSensitive(message, "role")),
                              String(cJSON_GetObjectItemCaseSensitive(message, "text"))};
                if (value.id.empty() || value.id.size() > 256 || value.text.size() > 4096 ||
                    (value.role != "user" && value.role != "codex")) continue;
                cJSON* part = nullptr;
                cJSON_ArrayForEach(part, cJSON_GetObjectItemCaseSensitive(message, "content")) {
                    if (value.content.size() >= 20) break;
                    Message::Part content;
                    content.type = BoundedString(part, "type", 16);
                    if (content.type == "text") {
                        content.text = BoundedString(part, "text", 4096);
                        if (content.text.empty()) continue;
                    } else if (content.type == "image") {
                        content.media_id = BoundedString(part, "mediaId", 192);
                        content.alt = BoundedString(part, "alt", 512);
                        if (content.media_id.empty()) continue;
                        const cJSON* width = cJSON_GetObjectItemCaseSensitive(part, "width");
                        const cJSON* height = cJSON_GetObjectItemCaseSensitive(part, "height");
                        content.width = cJSON_IsNumber(width) ? width->valueint : 0;
                        content.height = cJSON_IsNumber(height) ? height->valueint : 0;
                    } else continue;
                    value.content.push_back(std::move(content));
                }
                if (value.text.empty() && value.content.empty()) continue;
                parsed.conversation.messages.push_back(std::move(value));
                if (parsed.conversation.messages.size() > 10) parsed.conversation.messages.erase(parsed.conversation.messages.begin());
            }
        }
        cJSON* request = nullptr;
        cJSON_ArrayForEach(request, cJSON_GetObjectItemCaseSensitive(conversation, "interactions")) {
            if (parsed.conversation.interactions.size() >= 32) break;
            Interaction value;
            value.id = BoundedString(request, "id", 192);
            value.kind = BoundedString(request, "kind", 32);
            value.title = BoundedString(request, "title", 512);
            value.body = BoundedString(request, "body", 8192);
            value.status = BoundedString(request, "status", 32);
            value.response_text = BoundedString(request, "responseText", 4096);
            value.error = BoundedString(request, "error", 1024);
            value.after_message_id = BoundedString(request, "afterMessageId", 256);
            value.blocking = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(request, "blocking"));
            value.can_respond = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(request, "canRespond"));
            if (value.id.empty() || (value.kind != "approval" && value.kind != "question")) continue;
            value.options = ParseOptions(cJSON_GetObjectItemCaseSensitive(request, "options"), 12);
            cJSON* question = nullptr;
            cJSON_ArrayForEach(question, cJSON_GetObjectItemCaseSensitive(request, "questions")) {
                if (value.questions.size() >= 8) break;
                InteractionQuestion q;
                q.id = BoundedString(question, "id", 192);
                q.header = BoundedString(question, "header", 512);
                q.question = BoundedString(question, "question", 4096);
                q.allow_free_text = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(question, "allowFreeText"));
                q.multiple = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(question, "multiple"));
                q.is_secret = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(question, "isSecret"));
                q.options = ParseOptions(cJSON_GetObjectItemCaseSensitive(question, "options"), 24);
                if (!q.id.empty() && !q.question.empty()) value.questions.push_back(std::move(q));
            }
            char* signature = cJSON_PrintUnformatted(request);
            if (signature) { value.signature = signature; cJSON_free(signature); }
            parsed.conversation.interactions.push_back(std::move(value));
        }
    }
    const cJSON* models = cJSON_GetObjectItemCaseSensitive(root, "models");
    cJSON_ArrayForEach(entry, models) {
        Model model{String(cJSON_GetObjectItemCaseSensitive(entry, "id")), String(cJSON_GetObjectItemCaseSensitive(entry, "label"))};
        model.fast_supported = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "fastSupported"));
        cJSON* effort = nullptr;
        cJSON_ArrayForEach(effort, cJSON_GetObjectItemCaseSensitive(entry, "efforts")) {
            model.efforts.push_back({String(cJSON_GetObjectItemCaseSensitive(effort, "id")), String(cJSON_GetObjectItemCaseSensitive(effort, "label"))});
        }
        if (!model.id.empty()) parsed.models.push_back(std::move(model));
    }
    *out = std::move(parsed); return true;
}
}  // namespace
bool ParseStateRoot(const cJSON* root, State* out, std::string* error) {
    return out != nullptr && ParseRoot(root, out, error);
}
bool ParseStateJson(const std::string& json, State* out, std::string* error) {
    if (!out) return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json.c_str()), cJSON_Delete);
    return ParseStateRoot(root.get(), out, error);
}
const Slot* SelectedTask(const State& state) {
    if (state.selected_slot >= 0 && state.selected_slot < 6) {
        const Slot& slot = state.slots[state.selected_slot];
        if (IsBound(slot)) return &slot;
    }
    return IsBound(state.active_task) ? &state.active_task : nullptr;
}
bool MatchesSelectedTask(const State& state, const std::string& host_id, const std::string& thread_id) {
    const Slot* target = SelectedTask(state);
    return target != nullptr && !host_id.empty() && !thread_id.empty() &&
           target->host_id == host_id && target->thread_id == thread_id;
}
const Slot* SettingsTarget(const State& state) {
    if (const auto* task = SelectedTask(state)) return task;
    return !SelectedTask(state) && !state.draft_request_id.empty() ? &state.draft_settings : nullptr;
}
void TaskEntrySelection::Remember(const State& state) {
    if (pending_ || !state.connected) return;
    const Slot* target = SelectedTask(state);
    if (!target || target->host_id.empty()) return;
    host_id_ = target->host_id;
    thread_id_ = target->thread_id;
}
const Slot* TaskEntrySelection::Resolve(const State& state) const {
    if (!pending_ || !state.connected) return nullptr;
    if (!state.draft_request_id.empty()) return nullptr;
    for (const auto& slot : state.slots) {
        if (IsBound(slot) && slot.host_id == host_id_ && slot.thread_id == thread_id_) return &slot;
    }
    // A newly created task can still be current outside the six listed slots.
    const Slot* current = SelectedTask(state);
    if (current && current->host_id == host_id_ && current->thread_id == thread_id_) return current;
    for (const auto& slot : state.slots) {
        if (IsBound(slot) && !slot.host_id.empty()) return &slot;
    }
    return nullptr;
}
bool ApplyStateRoot(const cJSON* root, State* state, std::string* error) {
    if (!state) return false;
    State next;
    if (!ParseStateRoot(root, &next, error)) return false;
    if (!next.stream_id.empty() && next.stream_id == state->stream_id && next.revision < state->revision) return false;
    *state = std::move(next); return true;
}
bool ApplyStateJson(const std::string& json, State* state, std::string* error) {
    if (!state) return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json.c_str()), cJSON_Delete);
    return ApplyStateRoot(root.get(), state, error);
}
bool ApplyActionResultRoot(const cJSON* root, State* state, std::string* error) {
    if (!state || !root) return false;
    const bool valid = std::strcmp(String(cJSON_GetObjectItemCaseSensitive(root, "type")), "codex_action_result") == 0;
    const bool success = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "success"));
    const std::string remote_error = BoundedString(root, "error", 512);
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!valid || !success) {
        // Rejections may include an authoritative snapshot.  This is
        // particularly important for a rejected optimistic new_task: restore
        // the previous selected task instead of leaving a local placeholder.
        if (payload) {
            State next;
            if (ParseStateRoot(payload, &next, nullptr) &&
                (next.stream_id.empty() || next.stream_id != state->stream_id || next.revision >= state->revision)) {
                *state = std::move(next);
            }
        }
        SetError(error, remote_error.empty() ? "action rejected" : remote_error.c_str());
        return false;
    }
    if (!payload) return true;
    // A successful result may arrive after a fresher stream snapshot. Finish
    // the request without rolling the selected conversation back.
    State next;
    const bool parsed = ParseStateRoot(payload, &next, error);
    if (parsed && (next.stream_id.empty() || next.stream_id != state->stream_id || next.revision >= state->revision)) *state = std::move(next);
    return parsed;
}
bool ApplyActionResultJson(const std::string& json, State* state, std::string* error) {
    if (!state) return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json.c_str()), cJSON_Delete);
    return ApplyActionResultRoot(root.get(), state, error);
}
void BeginDraft(State* state, const std::string& request_id) {
    if (!state || request_id.empty()) return;
    state->selected_slot = -1;
    state->active_task = {};
    state->draft_request_id = request_id;
    state->draft_status = "preparing";
    state->draft_binding = false;
    state->draft_settings_loading = true;
    state->draft_submitted = false;
    state->draft_settings = {};
    state->draft_settings.host_id = "local";
    state->draft_settings.title = "新任务";
    state->draft_settings_error.clear();
    state->conversation = {};
}
void FailDraft(State* state, const std::string& message) {
    if (!state || state->draft_request_id.empty()) return;
    state->draft_status = "error";
    state->draft_binding = false;
    state->draft_settings_loading = false;
    state->draft_settings_error = message.empty() ? "新任务准备失败，请重试" : message;
}
bool IsBound(const Slot& slot) { return slot.state != SlotState::Unbound && !slot.thread_id.empty(); }
bool IsBusy(const Slot& slot) { return slot.state == SlotState::Working || slot.state == SlotState::Waiting; }
const Model* FindModel(const State& state, const std::string& id) { for (const auto& model : state.models) if (model.id == id) return &model; return nullptr; }
std::string StateLabel(SlotState state) { switch (state) { case SlotState::Unbound: return "未绑定"; case SlotState::Idle: return "空闲"; case SlotState::Working: return "执行中"; case SlotState::Waiting: return "等待操作"; case SlotState::Error: return "错误"; default: return "待同步"; } }
std::string BuildActionJson(const std::string& request_id, const char* action, int slot,
                            const std::string& model, const std::string& effort, int fast,
                            const std::string& thread_id, const std::string& host_id,
                            const std::string& draft_id, const std::string& stream_id) {
    if (request_id.empty() || !action || !*action) return {};
    std::string json = "{\"type\":\"codex_action\",\"request_id\":\"" + Escape(request_id) + "\",\"action\":\"" + Escape(action) + "\"";
    if (slot >= 0 && slot < 6) json += ",\"slot\":" + std::to_string(slot);
    else if (!thread_id.empty() && !stream_id.empty()) json += ",\"slot\":-1,\"stream_id\":\"" + Escape(stream_id) + "\"";
    if (!thread_id.empty()) json += ",\"thread_id\":\"" + Escape(thread_id) + "\"";
    if (!host_id.empty()) json += ",\"host_id\":\"" + Escape(host_id) + "\"";
    if (!draft_id.empty()) {
        if (slot >= 0 || !thread_id.empty() || stream_id.empty()) return {};
        json += ",\"draft_id\":\"" + Escape(draft_id) + "\",\"stream_id\":\"" + Escape(stream_id) + "\"";
    }
    if (!model.empty()) json += ",\"model\":\"" + Escape(model) + "\"";
    if (!effort.empty()) json += ",\"effort\":\"" + Escape(effort) + "\"";
    if (fast >= 0) json += std::string(",\"fast\":") + (fast ? "true" : "false");
    return json + "}";
}
}  // namespace agent_ui::codex_menu
