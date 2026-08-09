#pragma once

#include <cstdint>

namespace agent_ui::expression_spec {

enum class Action : uint8_t {
    Idle = 0,
    Smile,
    Laugh,
    Yawn,
    Listening,
    Answering,
    Charging,
    Complete,
    Sleep,
    Wake,
    Dizzy,
};

enum class Energy : uint8_t {
    Normal = 0,
    Tired,
    Exhausted,
};

inline constexpr uint32_t kFramesPerSecond = 30;
inline constexpr uint32_t kFrameDelayMs = 1000 / kFramesPerSecond;
inline constexpr uint32_t kDirectMorphMs = 220;
inline constexpr float kMotionDurationSeconds = 5.4f;

constexpr float DurationSeconds(Action action) {
    switch (action) {
        case Action::Smile: return 1.0f;
        case Action::Laugh: return 2.0f;
        case Action::Yawn: return 3.0f;
        case Action::Charging: return 1.8f;
        case Action::Complete: return 1.5f;
        case Action::Wake: return 0.9f;
        case Action::Dizzy: return 2.6f;
        default: return 0.0f;
    }
}

constexpr float EnvelopeSeconds(Action action) {
    switch (action) {
        case Action::Smile: return 1.0f;
        case Action::Laugh: return 2.0f;
        case Action::Yawn: return 3.0f;
        case Action::Listening: return 1.6f;
        case Action::Answering: return 1.4f;
        case Action::Charging: return 1.8f;
        case Action::Complete: return 1.5f;
        case Action::Wake: return 0.9f;
        case Action::Dizzy: return 2.6f;
        default: return 0.0f;
    }
}

constexpr bool IsLooping(Action action) {
    return action == Action::Listening ||
           action == Action::Answering ||
           action == Action::Sleep;
}

}  // namespace agent_ui::expression_spec
