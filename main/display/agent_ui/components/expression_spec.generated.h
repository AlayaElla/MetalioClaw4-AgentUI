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
    // Imported from the IrisOLED expression sheet: single-purpose poses the
    // player only holds long enough to breathe through them.
    Connecting,
    WinkLeft,
    WinkRight,
    BlinkUp,
    BlinkDown,
    LookLeft,
    LookRight,
    LookUp,
    LookDown,
    Surprised,
    Bored,
    Sad,
    Angry,
    Scared,
    Despair,
    Furious,
    Alert,
    // Whole-panel status graphics imported from the IrisOLED special-expression
    // bitmaps. While one of these plays it replaces the eye pair entirely.
    Battery,
    BatteryFull,
    BatteryLow,
    Warning,
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
        case Action::WinkLeft:
        case Action::WinkRight: return 1.1f;
        case Action::BlinkUp:
        case Action::BlinkDown: return 0.8f;
        case Action::LookLeft:
        case Action::LookRight:
        case Action::LookUp:
        case Action::LookDown: return 1.7f;
        case Action::Surprised: return 1.2f;
        case Action::Bored: return 2.4f;
        case Action::Sad: return 2.6f;
        case Action::Angry: return 2.0f;
        case Action::Scared: return 1.3f;
        case Action::Despair: return 2.8f;
        case Action::Furious: return 2.2f;
        case Action::Alert: return 1.0f;
        // Battery graphics are announcements, so they time out on their own.
        // Warning has no duration: it belongs to the fault state and holds until
        // the state moves elsewhere.
        case Action::Battery:
        case Action::BatteryFull: return 2.2f;
        case Action::BatteryLow: return 2.6f;
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
        case Action::Connecting: return 1.6f;
        case Action::WinkLeft:
        case Action::WinkRight: return 1.1f;
        case Action::BlinkUp:
        case Action::BlinkDown: return 0.4f;
        case Action::LookLeft:
        case Action::LookRight:
        case Action::LookUp:
        case Action::LookDown: return 1.7f;
        case Action::Surprised: return 1.2f;
        case Action::Bored: return 2.4f;
        case Action::Sad: return 2.6f;
        case Action::Angry: return 2.0f;
        case Action::Scared: return 0.9f;
        case Action::Despair: return 2.2f;
        case Action::Furious: return 1.6f;
        case Action::Alert: return 0.5f;
        case Action::Battery:
        case Action::BatteryFull: return 2.2f;
        case Action::BatteryLow: return 2.6f;
        // Warning belongs to a fault state that can outlast the animation, so the
        // pulse repeats for as long as the state is held.
        case Action::Warning: return 1.6f;
        default: return 0.0f;
    }
}

constexpr bool IsLooping(Action action) {
    return action == Action::Listening ||
           action == Action::Answering ||
           action == Action::Sleep ||
           action == Action::Connecting ||
           action == Action::Warning;
}

}  // namespace agent_ui::expression_spec
