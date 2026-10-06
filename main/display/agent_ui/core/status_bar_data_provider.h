#pragma once

#include <array>
#include <atomic>
#include <string>

#include "agent_ui_types.h"
#include "battery_snapshot.h"
#include "status_bar_state.h"

namespace agent_ui {

struct StatusBarRawReadings {
    bool ai_available = true;
    bool bluetooth_enabled = false;
    bool bluetooth_connected = false;
    const char* network_icon = nullptr;
    std::array<char, 16> time_text{{'-', '-', ':', '-', '-', '\0'}};
    bool activation_pending = false;
    std::string activation_code;

    bool cached_battery_supported = false;
    bool cached_battery_read = false;
    BatterySnapshot battery_snapshot{};
    bool legacy_battery_available = false;
    int legacy_battery_level = 0;
    bool legacy_battery_charging = false;
};

struct StatusBarBatteryData {
    bool has_battery = false;
    int level = 0;
    bool charging = false;
};

struct StatusBarData {
    bool ai_available = true;
    bool bluetooth_enabled = false;
    bool bluetooth_connected = false;
    NetworkMode network_mode = NetworkMode::Wifi;
    StatusBarNetworkIcon network_icon = StatusBarNetworkIcon::WifiDefault;
    std::array<char, 16> time_text{{'-', '-', ':', '-', '-', '\0'}};
    bool activation_pending = false;
    std::string activation_code;
    StatusBarBatteryData battery;
};

// Data sources expose snapshots only. They never return LVGL-owned objects;
// the network icon token is consumed while Collect() is running.
class StatusBarDataSource {
public:
    virtual ~StatusBarDataSource() = default;
    virtual StatusBarRawReadings ReadCurrent() = 0;
    virtual NetworkMode ReadNetworkMode() = 0;
    virtual bool IsAiAvailableNow() = 0;
};

class StatusBarDataProvider {
public:
    explicit StatusBarDataProvider(StatusBarDataSource& source) : source_(source) {}

    // Deliberately has no force parameter. A forced UI repaint must not cause a
    // settings read or clear a dirty notification that arrived during a read.
    StatusBarData Collect();
    bool IsAiAvailableNow() { return source_.IsAiAvailableNow(); }
    void InvalidateNetworkModeCache() {
        network_mode_dirty_.store(true, std::memory_order_release);
    }
    NetworkMode network_mode() const { return network_mode_; }

private:
    StatusBarDataSource& source_;
    std::atomic<bool> network_mode_dirty_{true};
    NetworkMode network_mode_ = NetworkMode::Wifi;
    bool last_battery_charging_ = false;
};

StatusBarDataSource& GetSystemStatusBarDataSource();

}  // namespace agent_ui
