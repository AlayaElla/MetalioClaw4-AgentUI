#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agent_ui::bluetooth {

enum class Lifecycle {
    Load,
    Unload,
    Suspend,
    Resume,
};

enum class ConnectionState : uint8_t {
    Idle,
    Scanning,
    Connecting,
    Connected,
};

enum class AudioProfile : uint8_t {
    None,
    Music,
    Call,
};

struct Device {
    std::string address;
    std::string name;

    bool operator==(const Device& other) const {
        return address == other.address && name == other.name;
    }
};

enum class IntentType {
    SetEnabled,
    Scan,
    Connect,
    Reset,
    SetAudioProfile,
};

struct Intent {
    IntentType type = IntentType::SetEnabled;
    bool enabled = false;
    std::size_t index = 0;
    AudioProfile audio_profile = AudioProfile::None;

    static Intent SetEnabled(bool value) {
        return {.type = IntentType::SetEnabled, .enabled = value};
    }

    static Intent Scan() { return {.type = IntentType::Scan}; }

    static Intent Connect(std::size_t value) {
        return {.type = IntentType::Connect, .index = value};
    }

    static Intent Reset() { return {.type = IntentType::Reset}; }

    static Intent SetAudioProfile(AudioProfile profile) {
        return {.type = IntentType::SetAudioProfile,
                .audio_profile = profile};
    }
};

enum class CommandType {
    Start,
    Stop,
    SetEnabled,
    Scan,
    Connect,
    Reset,
    SetAudioProfile,
};

struct Command {
    CommandType type = CommandType::Start;
    bool enabled = false;
    std::size_t index = 0;
    AudioProfile audio_profile = AudioProfile::None;
};

enum class EventType {
    Snapshot,
};

struct Event {
    EventType type = EventType::Snapshot;
    bool enabled = false;
    bool scanning = false;
    bool resetting = false;
    ConnectionState connection = ConnectionState::Idle;
    AudioProfile audio_profile = AudioProfile::None;
    bool has_current_device = false;
    Device current_device;
    std::vector<Device> nearby_devices;
};

}  // namespace agent_ui::bluetooth
