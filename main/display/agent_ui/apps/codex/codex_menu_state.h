#pragma once

#include <array>
#include <string>
#include <vector>

namespace agent_ui::codex_menu {

enum class SlotState { Unbound, Unknown, Idle, Working, Waiting, Error };

struct Effort { std::string id; std::string label; };
struct Model { std::string id; std::string label; std::vector<Effort> efforts; bool fast_supported = false; };
struct Slot {
    int slot = -1;
    std::string host_id, thread_id, title, model, effort, service_tier;
    SlotState state = SlotState::Unknown;
    bool synced = false;
    // Older bridge snapshots do not carry the field-level readiness flags.
    // In that case parsing falls back to `synced`, preserving their behavior.
    bool model_ready = true;
    bool effort_ready = true;
    bool fast_ready = true;
    bool has_fast = false;
    bool fast = false;
    int64_t updated_at = 0;
};
struct Message {
    std::string id, role, text;
    struct Part {
        std::string type, text, media_id, alt;
        int width = 0, height = 0;
        bool operator==(const Part& o) const { return type == o.type && text == o.text && media_id == o.media_id && alt == o.alt && width == o.width && height == o.height; }
    };
    std::vector<Part> content;
    bool operator==(const Message& other) const { return id == other.id && role == other.role && text == other.text && content == other.content; }
};
struct InteractionOption { std::string id, label, description; };
struct InteractionQuestion {
    std::string id, header, question;
    std::vector<InteractionOption> options;
    bool allow_free_text = true, multiple = false, is_secret = false;
};
struct Interaction {
    std::string id, kind, title, body, status, response_text, error, after_message_id;
    std::vector<InteractionOption> options;
    std::vector<InteractionQuestion> questions;
    // Native option pickers and desktop-only async widgets are visible on the
    // device but must never submit a fabricated response from it.
    bool blocking = true, can_respond = true;
    std::string signature;
    bool operator==(const Interaction& o) const { return signature == o.signature; }
};
struct Conversation {
    std::string host_id, thread_id;
    bool ready = false;
    std::vector<Message> messages;
    std::vector<Interaction> interactions;
};
struct State {
    int version = 0;
    uint64_t revision = 0;
    bool connected = false;
    std::string source;
    std::string stream_id;
    std::string draft_request_id;
    std::string draft_status;
    bool draft_binding = false;
    bool draft_settings_loading = false;
    bool draft_submitted = false;
    Slot draft_settings{};
    std::string draft_settings_error;
    Conversation conversation;
    int selected_slot = -1;
    std::array<Slot, 6> slots{};
    // A newly created task can be current before it is assigned to a six-slot
    // control. Its identity also addresses conversation and settings updates.
    Slot active_task{};
    std::vector<Model> models;
    bool can_select_task = false, can_new_task = false, can_set_model = false;
    bool can_set_effort = false, can_set_fast = false;
};

// Lives beyond the page's LVGL objects. Remember identity, since slot order can
// change while the page is closed. A new boot starts with the first bound task.
class TaskEntrySelection {
public:
    void Begin() { pending_ = true; }
    void Complete() { pending_ = false; }
    void Remember(const State& state);
    const Slot* Resolve(const State& state) const;

private:
    bool pending_ = false;
    std::string host_id_, thread_id_;
};

// Parsing and message construction have no LVGL, NVS, or transport dependency.
bool ParseStateJson(const std::string& json, State* out, std::string* error = nullptr);
bool ApplyStateJson(const std::string& json, State* state, std::string* error = nullptr);
const Slot* SelectedTask(const State& state);
const Slot* SettingsTarget(const State& state);
bool MatchesSelectedTask(const State& state, const std::string& host_id, const std::string& thread_id);
bool ApplyActionResultJson(const std::string& json, State* state,
                           std::string* error = nullptr);
// Optimistic device-only state used from a successfully sent new_task action
// until the bridge publishes the authoritative draft identity/state.
void BeginDraft(State* state, const std::string& request_id);
void FailDraft(State* state, const std::string& message);
bool IsBound(const Slot& slot);
bool IsBusy(const Slot& slot);
const Model* FindModel(const State& state, const std::string& id);
std::string StateLabel(SlotState state);
std::string BuildActionJson(const std::string& request_id, const char* action,
                            int slot = -1, const std::string& model = {},
                            const std::string& effort = {}, int fast = -1,
                            const std::string& thread_id = {},
                            const std::string& host_id = {},
                            const std::string& draft_id = {},
                            const std::string& stream_id = {});

}  // namespace agent_ui::codex_menu
