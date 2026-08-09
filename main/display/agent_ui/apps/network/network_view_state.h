#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "network_contract.h"

namespace agent_ui::network {

struct ViewState {
    bool mounted = false;
    bool cellular = false;
    bool external_slot = false;
    int selected_mode = 0;
    bool scanning = false;
    bool scan_started = false;
    bool connecting = false;
    bool network_switch_pending = false;
    bool sim_switch_pending = false;

    std::vector<SavedNetwork> saved_networks;
    std::vector<NearbyNetwork> nearby_networks;
    std::string status;
    uint32_t status_color = 0;
    Dialog dialog = Dialog::None;
    std::string dialog_ssid;
    std::string dialog_title;
    std::string dialog_detail;
    int restart_remaining = 0;
    uint32_t failure_auto_close_ms = 0;

    bool operator==(const ViewState& other) const {
        return mounted == other.mounted && cellular == other.cellular &&
               external_slot == other.external_slot &&
               selected_mode == other.selected_mode && scanning == other.scanning &&
               scan_started == other.scan_started && connecting == other.connecting &&
               network_switch_pending == other.network_switch_pending &&
               sim_switch_pending == other.sim_switch_pending &&
               saved_networks == other.saved_networks &&
               nearby_networks == other.nearby_networks && status == other.status &&
               status_color == other.status_color && dialog == other.dialog &&
               dialog_ssid == other.dialog_ssid && dialog_title == other.dialog_title &&
               dialog_detail == other.dialog_detail &&
               restart_remaining == other.restart_remaining &&
               failure_auto_close_ms == other.failure_auto_close_ms;
    }

    bool operator!=(const ViewState& other) const { return !(*this == other); }
};

}  // namespace agent_ui::network
