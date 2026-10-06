#pragma once

namespace agent_ui {

class PowerKey {
public:
    static void Initialize();
    // Reuse the existing worker; never block LVGL on radio/audio handshakes.
    static bool ResumeStandbyPeripherals();
    static void NotifyStandbyStarted();
    // Enqueue sampler quiescence and board standby work on the power worker.
    // This call is safe from the LVGL owner and never waits for hardware.
    static void RequestStandbyEntry();
};

}  // namespace agent_ui
