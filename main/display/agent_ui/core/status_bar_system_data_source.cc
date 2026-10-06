#include "status_bar_data_provider.h"

#include <ctime>

#include "ai/ai_availability.h"
#include "application.h"
#include "apps/bluetooth/bluetooth_adapter.h"
#include "board.h"
#include "dual_network_board.h"
#include "settings.h"

namespace agent_ui {
namespace {

class SystemStatusBarDataSource final : public StatusBarDataSource {
public:
    StatusBarRawReadings ReadCurrent() override {
        StatusBarRawReadings readings;
        readings.ai_available = ai::Availability::Get().IsAvailable();

        auto& bt_adapter = bluetooth::Adapter::Get();
        readings.bluetooth_enabled = bt_adapter.IsEnabled();
        readings.bluetooth_connected = readings.bluetooth_enabled &&
                                       bt_adapter.IsConnected();

        Board& board = Board::GetInstance();
        readings.network_icon = board.GetNetworkStateIcon();

        char time_buffer[16] = "--:--";
        const time_t now = time(nullptr);
        struct tm local = {};
        if (localtime_r(&now, &local) != nullptr && local.tm_year >= 125) {
            strftime(time_buffer, sizeof(time_buffer), "%H:%M", &local);
        }
        for (std::size_t index = 0; index < readings.time_text.size(); ++index) {
            readings.time_text[index] = time_buffer[index];
            if (time_buffer[index] == '\0') break;
        }

        const auto& app = Application::GetInstance();
        readings.activation_pending = app.HasPendingActivation();
        if (readings.activation_pending) {
            readings.activation_code = app.GetPendingActivationCode();
        }

        readings.cached_battery_supported = board.SupportsCachedBatterySnapshot();
        if (readings.cached_battery_supported) {
            readings.cached_battery_read = board.GetBatterySnapshot(
                readings.battery_snapshot);
        } else {
            bool discharging = false;
            readings.legacy_battery_available = board.GetBatteryLevel(
                readings.legacy_battery_level,
                readings.legacy_battery_charging, discharging);
        }
        return readings;
    }

    NetworkMode ReadNetworkMode() override {
        const NetworkType type = DualNetworkBoard::LoadNetworkTypeFromSettings(1);
        if (type == NetworkType::WIFI) return NetworkMode::Wifi;

        Settings settings("network", true);
        return settings.GetInt("sim_slot", 0) == 1 ? NetworkMode::InternalSim
                                                    : NetworkMode::ExternalSim;
    }

    bool IsAiAvailableNow() override {
        return ai::Availability::Get().IsAvailable();
    }
};

}  // namespace

StatusBarDataSource& GetSystemStatusBarDataSource() {
    static SystemStatusBarDataSource source;
    return source;
}

}  // namespace agent_ui
