#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

namespace agent_ui::external_apps {

enum class MediaState : uint8_t {
    Idle = 0,
    Connecting = 1,
    Playing = 2,
    Paused = 3,
    Error = 4,
};

class MediaService {
public:
    static constexpr size_t kSpectrumBandCount = 12;

    static MediaService& Get();
    static MediaService* Existing();

    int Start(void* owner, const char* url);
    int Pause(void* owner);
    int Resume(void* owner);
    int Stop(void* owner);
    MediaState state(void* owner) const;
    int GetSpectrum(void* owner, uint8_t* levels, size_t count) const;
    int GetVolume(void* owner) const;
    int SetVolume(void* owner, int volume);

    void SuspendOwner(void* owner);
    void ResumeOwner(void* owner);
    void UnloadOwner(void* owner);
    // Host-level launch barrier. Cancels any stale successor and waits until
    // the previous GMF/HLS pipeline has released its resources.
    bool ResetForAppLaunch(uint32_t timeout_ms);
    // Returns true when playback must be released asynchronously. The callback
    // runs off the UI thread, after the shared audio route is safe to acquire.
    bool BeginAssistantInteraction(std::function<void(bool)> ready);
    bool HasAssistantInteraction() const;
    void EndAssistantInteraction(bool resume = true);

private:
    MediaService();
    ~MediaService() = default;
    MediaService(const MediaService&) = delete;
    MediaService& operator=(const MediaService&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
    mutable std::mutex assistant_mutex_;
    void* assistant_owner_ = nullptr;
    uint32_t assistant_generation_ = 0;
    uint64_t assistant_epoch_ = 0;
};

}  // namespace agent_ui::external_apps
