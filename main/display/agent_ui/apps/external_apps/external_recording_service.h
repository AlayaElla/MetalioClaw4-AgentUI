#pragma once

#include <cstdint>

#include "metalio_app_api.h"

namespace agent_ui::external_apps {

class RecordingService {
public:
    static RecordingService& Get();
    static RecordingService* Existing();

    int Start(void* owner, const metalio_app_recording_config_t* config);
    int Stop(void* owner);
    int Cancel(void* owner);
    int GetStatus(void* owner, metalio_app_recording_status_t* status) const;

    void SuspendOwner(void* owner);
    void UnloadOwner(void* owner);
    // Host-level launch barrier. Stops an old recording and waits for its
    // writer task and buffers to be released.
    bool ResetForAppLaunch(uint32_t timeout_ms);

private:
    RecordingService();
    ~RecordingService() = default;
    RecordingService(const RecordingService&) = delete;
    RecordingService& operator=(const RecordingService&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace agent_ui::external_apps
