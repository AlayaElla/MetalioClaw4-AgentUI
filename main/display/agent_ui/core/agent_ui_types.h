#pragma once

#include <cstdint>

namespace agent_ui {

enum class ScreenId : uint8_t {
    Home = 0,
    Codex,
    Camera,
    Phone,
    Files,
    Settings,
    ExternalAppHost,
    DisplayDebug,
    Boot,
    Standby,
    Power,
};

enum class AgentState : uint8_t {
    Idle = 0,
    Connecting,
    Listening,
    Answering,
};

enum class NetworkMode : uint8_t {
    Wifi = 0,
    InternalSim,
    ExternalSim,
};

enum class AccentPreset : uint8_t {
    Cobalt = 0,
    Teal,
    Coral,
    Amber,
};

enum class AppearanceMode : uint8_t {
    Light = 0,
    Dark,
};

enum class TransitionDirection : uint8_t {
    Forward = 0,
    Back,
    Replace,
};

}  // namespace agent_ui
