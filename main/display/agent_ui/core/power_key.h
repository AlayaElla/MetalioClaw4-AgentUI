#pragma once

namespace agent_ui {

class PowerKey {
public:
    static void Initialize();
    // Reuse the existing worker; never block LVGL on radio/audio handshakes.
    static bool ResumeStandbyPeripherals();
    static void NotifyStandbyStarted();
};

}  // namespace agent_ui
