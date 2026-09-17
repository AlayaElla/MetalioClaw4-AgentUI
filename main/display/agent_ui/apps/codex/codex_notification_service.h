#pragma once

#include <cstdint>
#include <string>
#include <string_view>

struct cJSON;

namespace agent_ui::codex_notification {

enum class Kind : uint8_t { Attention, Success };

struct SettingsValue {
    bool enabled = true;
    uint8_t volume = 70;
};

// Page-independent notification coordinator. All state changes are serialized
// onto Application's main loop, so it is safe for the WebSocket receive path.
class Service {
public:
    static Service& GetInstance();

    // Returns true only for a syntactically valid codex_notification event.
    // The caller retains ownership of root.
    bool HandleMessage(const cJSON* root);
    void SetRecording(bool recording);

    SettingsValue GetSettings();
    void SetEnabled(bool enabled);
    void SetVolume(int volume);
    void Preview(Kind kind);

private:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    void EnsureLoaded();
    void OnEvent(std::string id, Kind kind, bool historical);
    void EnqueuePreview(Kind kind);
    void Pump();
    void ScheduleRetry();
    void PersistRecentIds();
};

inline Service& Get() { return Service::GetInstance(); }

}  // namespace agent_ui::codex_notification
