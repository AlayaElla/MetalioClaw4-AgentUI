#pragma once

#include <cstdint>

#include "agent_ui_types.h"

namespace agent_ui {

enum class StatusBarNetworkIcon : uint8_t {
    WifiDefault = 0,
    WifiDisconnected,
    WifiWeak,
    WifiFair,
    WifiGood,
    CellularOff,
    CellularWeak,
    CellularFair,
    CellularGood,
    CellularStrong,
};

struct StatusBarAgentInput {
    bool home_active = true;
    bool lock_screen = false;
    bool ai_available = true;
    bool activation_pending = false;
    bool reply_caption_present = false;
};

struct StatusBarAgentPresentation {
    bool show_activation = false;
    bool show_reply_caption = false;
    bool hide_face = true;
    bool hide_cluster = true;
    bool clear_reply_caption = false;
};

struct StatusBarBatteryPresentation {
    bool has_battery = false;
    bool charging = false;
    bool low = false;
    int level = 0;
    int active_cells = 0;
};

StatusBarNetworkIcon ResolveStatusBarNetworkIcon(NetworkMode mode,
                                                 const char* board_icon);
StatusBarAgentPresentation ResolveStatusBarAgentPresentation(
    const StatusBarAgentInput& input);
StatusBarBatteryPresentation ResolveStatusBarBatteryPresentation(
    bool has_battery, int level, bool charging);

}  // namespace agent_ui
