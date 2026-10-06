#include "codex_notification_service.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "assets/common_sounds.h"
#include "cJSON.h"
#include "codex_ws_client.h"
#include "settings.h"

namespace agent_ui::codex_notification {
namespace {

constexpr char kNamespace[] = "codexnotify";
constexpr char kEnabledKey[] = "enabled";
constexpr char kVolumeKey[] = "volume";
constexpr char kRecentKey[] = "recent";
constexpr size_t kRecentLimit = 20;
constexpr size_t kMaxIdLength = 64;
constexpr size_t kPendingLimit = 8;
constexpr int64_t kPendingTtlUs = 30LL * 1000 * 1000;
constexpr int64_t kRetryDelayUs = 250LL * 1000;

struct Pending {
    Kind kind;
    int64_t expires_at_us;
    bool preview = false;
};

class Impl {
public:
    std::recursive_mutex mutex;
    bool loaded = false;
    std::atomic<bool> recording{false};
    bool retry_scheduled = false;
    SettingsValue settings{};
    std::deque<std::string> recent_ids;
    std::deque<Pending> pending;
    esp_timer_handle_t retry_timer = nullptr;
};

Impl& State() {
    static Impl state;
    return state;
}

std::string EncodeRecentIds(const std::deque<std::string>& ids) {
    std::string result;
    for (const auto& id : ids) {
        result += std::to_string(id.size());
        result += ':';
        result += id;
    }
    return result;
}

std::deque<std::string> DecodeRecentIds(const std::string& encoded) {
    std::deque<std::string> ids;
    size_t offset = 0;
    while (offset < encoded.size() && ids.size() < kRecentLimit) {
        const size_t colon = encoded.find(':', offset);
        if (colon == std::string::npos || colon == offset) break;
        size_t length = 0;
        for (size_t i = offset; i < colon; ++i) {
            if (encoded[i] < '0' || encoded[i] > '9') return {};
            length = length * 10 + static_cast<size_t>(encoded[i] - '0');
            if (length > kMaxIdLength) return {};
        }
        const size_t start = colon + 1;
        if (start + length > encoded.size()) return {};
        ids.emplace_back(encoded.substr(start, length));
        offset = start + length;
    }
    return ids;
}

bool IsKnown(const std::deque<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

const std::string_view& ToneFor(Kind kind) {
    return kind == Kind::Attention ? CommonSounds::OGG_CODEX_ATTENTION
                                   : CommonSounds::OGG_CODEX_SUCCESS;
}

}  // namespace

Service::Service() { (void)State(); }

Service::~Service() {
    if (State().retry_timer != nullptr) esp_timer_delete(State().retry_timer);
}

Service& Service::GetInstance() {
    static Service service;
    return service;
}

void Service::EnsureLoaded() {
    std::lock_guard<std::recursive_mutex> lock(State().mutex);
    auto& state = State();
    if (state.loaded) return;
    Settings settings(kNamespace, false);
    state.settings.enabled = settings.GetBool(kEnabledKey, true);
    state.settings.volume = static_cast<uint8_t>(std::clamp<int32_t>(settings.GetInt(kVolumeKey, 70), 0, 100));
    state.recent_ids = DecodeRecentIds(settings.GetString(kRecentKey));
    state.loaded = true;
    esp_timer_create_args_t timer_args{};
    timer_args.callback = [](void*) {
        Application::GetInstance().Schedule([]() {
            std::lock_guard<std::recursive_mutex> lock(State().mutex);
            State().retry_scheduled = false;
            Service::GetInstance().Pump();
        });
    };
    timer_args.arg = nullptr;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "codex_notify";
    timer_args.skip_unhandled_events = true;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &state.retry_timer));
}

bool Service::HandleMessage(const cJSON* root) {
    const auto& client = CodexWsClient::GetInstance();
    return HandleMessage(root, client.GetAppSessionGeneration(),
                         client.GetConnectionGeneration(), client.GetConnectionEpoch());
}

bool Service::HandleMessage(const cJSON* root, uint32_t app_generation) {
    const auto& client = CodexWsClient::GetInstance();
    return HandleMessage(root, app_generation, client.GetConnectionGeneration(),
                         client.GetConnectionEpoch());
}

bool Service::HandleMessage(const cJSON* root, uint32_t app_generation,
                            uint32_t connection_generation, uint32_t connection_epoch) {
    if (root == nullptr) return false;
    const auto* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const auto* id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const auto* kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    const auto* historical = cJSON_GetObjectItemCaseSensitive(root, "historical");
    if (!cJSON_IsString(type) || std::string_view(type->valuestring) != "codex_notification" ||
        !cJSON_IsString(id) || id->valuestring == nullptr ||
        !cJSON_IsString(kind) || kind->valuestring == nullptr) return false;
    const size_t id_length = strnlen(id->valuestring, kMaxIdLength + 1);
    if (id_length == 0 || id_length > kMaxIdLength) return false;
    const std::string event_id(id->valuestring, id_length);
    const std::string_view kind_name(kind->valuestring);
    if (kind_name != "attention" && kind_name != "success") return false;
    const auto event_kind = kind_name == "attention" ? Kind::Attention : Kind::Success;
    Application::GetInstance().Schedule([event_id, event_kind, app_generation,
        connection_generation, connection_epoch,
        is_historical = cJSON_IsTrue(historical)]() {
        const auto& client = CodexWsClient::GetInstance();
        if (!client.IsAppActive() || client.GetAppSessionGeneration() != app_generation ||
            client.GetConnectionGeneration() != connection_generation ||
            client.GetConnectionEpoch() != connection_epoch) return;
        Service::GetInstance().OnEvent(event_id, event_kind, is_historical);
    });
    return true;
}

void Service::SetRecording(bool recording) {
    // Close the gate immediately. A queued older idle transition must never
    // reopen it after a newer recording has started.
    State().recording.store(recording, std::memory_order_release);
    Application::GetInstance().Schedule([]() {
        auto& state = State();
        Service::GetInstance().EnsureLoaded();
        if (!state.recording.load(std::memory_order_acquire)) Service::GetInstance().Pump();
    });
}

SettingsValue Service::GetSettings() {
    std::lock_guard<std::recursive_mutex> lock(State().mutex);
    EnsureLoaded();
    return State().settings;
}

void Service::SetEnabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(State().mutex);
    EnsureLoaded();
    auto& state = State();
    state.settings.enabled = enabled;
    if (!enabled) state.pending.clear();
    Settings settings(kNamespace, true);
    settings.SetBool(kEnabledKey, enabled);
    if (enabled) Pump();
}

void Service::SetVolume(int volume) {
    std::lock_guard<std::recursive_mutex> lock(State().mutex);
    EnsureLoaded();
    State().settings.volume = static_cast<uint8_t>(std::clamp(volume, 0, 100));
    Settings settings(kNamespace, true);
    settings.SetInt(kVolumeKey, State().settings.volume);
}

void Service::PersistRecentIds() {
    std::lock_guard<std::recursive_mutex> lock(State().mutex);
    Settings settings(kNamespace, true);
    settings.SetString(kRecentKey, EncodeRecentIds(State().recent_ids));
}

void Service::OnEvent(std::string id, Kind kind, bool historical) {
    std::lock_guard<std::recursive_mutex> lock(State().mutex);
    EnsureLoaded();
    auto& state = State();
    if (IsKnown(state.recent_ids, id)) return;
    state.recent_ids.push_back(std::move(id));
    while (state.recent_ids.size() > kRecentLimit) state.recent_ids.pop_front();
    PersistRecentIds();
    if (historical || !state.settings.enabled) return;
    if (state.pending.size() == kPendingLimit) state.pending.pop_front();
    state.pending.push_back({kind, esp_timer_get_time() + kPendingTtlUs, false});
    Pump();
}

void Service::EnqueuePreview(Kind kind) {
    std::lock_guard<std::recursive_mutex> lock(State().mutex);
    EnsureLoaded();
    auto& state = State();
    if (state.pending.size() == kPendingLimit) state.pending.pop_front();
    state.pending.push_back({kind, esp_timer_get_time() + kPendingTtlUs, true});
    Pump();
}

void Service::Preview(Kind kind) {
    Application::GetInstance().Schedule([kind]() { Service::GetInstance().EnqueuePreview(kind); });
}

void Service::ScheduleRetry() {
    std::lock_guard<std::recursive_mutex> lock(State().mutex);
    auto& state = State();
    if (state.retry_scheduled || state.pending.empty()) return;
    state.retry_scheduled = true;
    ESP_ERROR_CHECK(esp_timer_start_once(state.retry_timer, kRetryDelayUs));
}

void Service::Pump() {
    std::lock_guard<std::recursive_mutex> lock(State().mutex);
    EnsureLoaded();
    auto& state = State();
    const int64_t now = esp_timer_get_time();
    while (!state.pending.empty() && state.pending.front().expires_at_us <= now) {
        state.pending.pop_front();
    }
    if (state.pending.empty()) return;
    if (state.recording.load(std::memory_order_acquire)) {
        ScheduleRetry();
        return;
    }
    const auto& item = state.pending.front();
    if (Application::GetInstance().PlayCodexNotificationSound(
            ToneFor(item.kind), state.settings.volume)) {
        state.pending.pop_front();
        if (!state.pending.empty()) ScheduleRetry();
    } else {
        ScheduleRetry();
    }
}

}  // namespace agent_ui::codex_notification

// The WebSocket component is built independently from main and therefore
// cannot include the C++ service header. It only parses this bounded message
// then queues the real work onto Application's main loop; it never plays or
// waits for audio on the receive callback.
extern "C" bool codex_remote_handle_notification_root(const cJSON* root, uint32_t app_generation,
                                                       uint32_t connection_generation,
                                                       uint32_t connection_epoch) {
    return agent_ui::codex_notification::Get().HandleMessage(
        root, app_generation, connection_generation, connection_epoch);
}

extern "C" void codex_remote_handle_notification_json(const char* json) {
    if (json == nullptr) return;
    constexpr size_t kMaxIncomingJsonBytes = 2048;
    const size_t length = strnlen(json, kMaxIncomingJsonBytes + 1);
    if (length == 0 || length > kMaxIncomingJsonBytes) return;
    cJSON* root = cJSON_ParseWithLength(json, length);
    if (root == nullptr) return;
    const auto& client = CodexWsClient::GetInstance();
    agent_ui::codex_notification::Get().HandleMessage(
        root, client.GetAppSessionGeneration(), client.GetConnectionGeneration(),
        client.GetConnectionEpoch());
    cJSON_Delete(root);
}
