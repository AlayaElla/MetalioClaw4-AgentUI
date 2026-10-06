#pragma once

#include <cstdint>
#include <functional>

#include "metalio_app_api.h"

namespace agent_ui::external_apps {

class SynthService {
public:
    static SynthService& Get();
    static SynthService* Existing();

    bool Available() const;
    bool IsActive() const;

    int Start(void* owner);
    int Set(void* owner, const metalio_app_synth_params_t* params);
    int Stop(void* owner);
    int GetState(void* owner, metalio_app_synth_state_t* state) const;

    void SuspendOwner(void* owner);
    void UnloadOwner(void* owner);
    void Interrupt();
    bool ResetForAppLaunch(uint32_t timeout_ms);
    // Returns true when an active or starting synth must release audio first.
    // In that case ready is called from the resident worker after route restore.
    bool BeginAssistantInteraction(std::function<void(bool)> ready);

private:
    SynthService();
    ~SynthService() = default;
    SynthService(const SynthService&) = delete;
    SynthService& operator=(const SynthService&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace agent_ui::external_apps
