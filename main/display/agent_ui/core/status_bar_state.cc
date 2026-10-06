#include "status_bar_state.h"

#include <algorithm>
#include <cstring>

#include <font_awesome.h>

namespace agent_ui {
namespace {

bool IsEmpty(const char* value) {
    return value == nullptr || value[0] == '\0';
}

bool Matches(const char* value, const char* expected) {
    return value != nullptr && std::strcmp(value, expected) == 0;
}

int BatteryCellCount(bool has_battery, int level) {
    if (!has_battery || level < 20) return 0;
    if (level < 50) return 1;
    if (level < 80) return 2;
    return 3;
}

}  // namespace

StatusBarNetworkIcon ResolveStatusBarNetworkIcon(NetworkMode mode,
                                                 const char* board_icon) {
    if (mode == NetworkMode::Wifi) {
        if (IsEmpty(board_icon)) return StatusBarNetworkIcon::WifiDefault;
        if (Matches(board_icon, FONT_AWESOME_WIFI_SLASH)) {
            return StatusBarNetworkIcon::WifiDisconnected;
        }
        if (Matches(board_icon, FONT_AWESOME_WIFI_WEAK)) {
            return StatusBarNetworkIcon::WifiWeak;
        }
        if (Matches(board_icon, FONT_AWESOME_WIFI_FAIR)) {
            return StatusBarNetworkIcon::WifiFair;
        }
        if (Matches(board_icon, FONT_AWESOME_WIFI)) {
            return StatusBarNetworkIcon::WifiGood;
        }
        return StatusBarNetworkIcon::WifiDefault;
    }

    if (IsEmpty(board_icon) || Matches(board_icon, FONT_AWESOME_SIGNAL_OFF)) {
        return StatusBarNetworkIcon::CellularOff;
    }
    if (Matches(board_icon, FONT_AWESOME_SIGNAL_WEAK)) {
        return StatusBarNetworkIcon::CellularWeak;
    }
    if (Matches(board_icon, FONT_AWESOME_SIGNAL_FAIR)) {
        return StatusBarNetworkIcon::CellularFair;
    }
    if (Matches(board_icon, FONT_AWESOME_SIGNAL_GOOD)) {
        return StatusBarNetworkIcon::CellularGood;
    }
    if (Matches(board_icon, FONT_AWESOME_SIGNAL_STRONG)) {
        return StatusBarNetworkIcon::CellularStrong;
    }
    return StatusBarNetworkIcon::CellularOff;
}

StatusBarAgentPresentation ResolveStatusBarAgentPresentation(
    const StatusBarAgentInput& input) {
    StatusBarAgentPresentation result;
    result.show_activation = input.activation_pending && !input.lock_screen;
    result.show_reply_caption = input.reply_caption_present && !input.lock_screen &&
                                input.ai_available && !result.show_activation;
    result.hide_face = input.home_active || input.lock_screen ||
                       !input.ai_available || result.show_activation;
    result.hide_cluster = input.lock_screen ||
                          (input.home_active && !result.show_activation);
    if (result.show_reply_caption) result.hide_cluster = false;
    result.clear_reply_caption = result.show_activation;
    return result;
}

StatusBarBatteryPresentation ResolveStatusBarBatteryPresentation(
    bool has_battery, int level, bool charging) {
    StatusBarBatteryPresentation result;
    result.has_battery = has_battery;
    result.charging = charging;
    result.level = has_battery ? std::clamp(level, 0, 100) : 0;
    result.low = has_battery && !charging && result.level < 20;
    result.active_cells = BatteryCellCount(has_battery, result.level);
    return result;
}

}  // namespace agent_ui
