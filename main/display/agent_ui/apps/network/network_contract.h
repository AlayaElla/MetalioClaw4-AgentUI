#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agent_ui::network {

struct SavedNetwork {
    std::string ssid;
    bool is_default = false;

    bool operator==(const SavedNetwork& other) const {
        return ssid == other.ssid && is_default == other.is_default;
    }
};

struct NearbyNetwork {
    std::string ssid;
    std::string detail;

    bool operator==(const NearbyNetwork& other) const {
        return ssid == other.ssid && detail == other.detail;
    }
};

enum class Lifecycle {
    Load,
    Unload,
    Suspend,
    Resume,
};

enum class Dialog {
    None,
    Password,
    Connecting,
    Failure,
    Restart,
    NetworkSwitch,
    SimSwitch,
};

enum class IntentType {
    Scan,
    ConnectSaved,
    RequestPassword,
    SubmitPassword,
    SelectMode,
    SelectSimSlot,
    DismissStatus,
};

struct Intent {
    IntentType type = IntentType::Scan;
    std::size_t index = 0;
    int value = 0;
    std::string text;
    std::string password;

    static Intent Scan() { return {.type = IntentType::Scan}; }

    static Intent ConnectSaved(std::size_t item_index) {
        return {.type = IntentType::ConnectSaved, .index = item_index};
    }

    static Intent RequestPassword(std::size_t item_index,
                                  const std::string& ssid) {
        return {.type = IntentType::RequestPassword,
                .index = item_index,
                .text = ssid};
    }

    static Intent SubmitPassword(const std::string& ssid,
                                 const std::string& value) {
        return {.type = IntentType::SubmitPassword,
                .text = ssid,
                .password = value};
    }

    static Intent SelectMode(int mode) {
        return {.type = IntentType::SelectMode, .value = mode};
    }

    static Intent SelectSimSlot(int slot) {
        return {.type = IntentType::SelectSimSlot, .value = slot};
    }

    static Intent DismissStatus() {
        return {.type = IntentType::DismissStatus};
    }
};

enum class CommandType {
    Start,
    Stop,
    Scan,
    ConnectSaved,
    Connect,
    SwitchNetwork,
    QuerySimSlot,
    SwitchSimSlot,
};

struct Command {
    CommandType type = CommandType::Start;
    std::size_t index = 0;
    int value = 0;
    std::string ssid;
    std::string password;
};

enum class EventType {
    ModeSnapshot,
    SavedNetworks,
    NearbyNetworks,
    Status,
    ScanStarted,
    ScanFinished,
    ConnectStarted,
    ConnectSucceeded,
    ConnectFailed,
    NetworkSwitchStarted,
    SimSwitchStarted,
    SimSwitchSucceeded,
    RestartCountdown,
    SimSlotSynced,
};

struct Event {
    EventType type = EventType::Status;
    bool cellular = false;
    bool external_slot = false;
    bool scanning = false;
    bool scan_started = false;
    int value = 0;
    uint32_t color = 0;
    uint32_t auto_close_ms = 0;
    std::string ssid;
    std::string text;
    std::string detail;
    std::vector<SavedNetwork> saved_networks;
    std::vector<NearbyNetwork> nearby_networks;
};

}  // namespace agent_ui::network
