#include "status_bar_data_provider.h"

namespace agent_ui {

StatusBarData StatusBarDataProvider::Collect() {
    const StatusBarRawReadings raw = source_.ReadCurrent();

    // Exchange before invoking the potentially slower settings reader. An
    // invalidation during that read remains set for the next refresh.
    if (network_mode_dirty_.exchange(false, std::memory_order_acq_rel)) {
        network_mode_ = source_.ReadNetworkMode();
    }

    StatusBarData data;
    data.ai_available = raw.ai_available;
    data.bluetooth_enabled = raw.bluetooth_enabled;
    data.bluetooth_connected = raw.bluetooth_enabled && raw.bluetooth_connected;
    data.network_mode = network_mode_;
    data.network_icon = ResolveStatusBarNetworkIcon(network_mode_, raw.network_icon);
    data.time_text = raw.time_text;
    data.activation_pending = raw.activation_pending;
    if (raw.activation_pending) data.activation_code = raw.activation_code;

    if (raw.cached_battery_supported) {
        const BatterySnapshot& snapshot = raw.battery_snapshot;
        data.battery.has_battery = raw.cached_battery_read && snapshot.fresh;
        data.battery.level = data.battery.has_battery ? snapshot.level : 0;
        data.battery.charging = data.battery.has_battery || snapshot.has_data
                                    ? snapshot.charging
                                    : last_battery_charging_;
    } else {
        data.battery.has_battery = raw.legacy_battery_available;
        data.battery.level = raw.legacy_battery_level;
        data.battery.charging = raw.legacy_battery_charging;
    }
    last_battery_charging_ = data.battery.charging;
    return data;
}

}  // namespace agent_ui
