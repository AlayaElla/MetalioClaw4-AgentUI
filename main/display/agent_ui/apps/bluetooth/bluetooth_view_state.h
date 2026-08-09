#pragma once

#include "bluetooth_contract.h"

namespace agent_ui::bluetooth {

struct ViewState {
    bool mounted = false;
    bool enabled = false;
    bool scanning = false;
    bool resetting = false;
    ConnectionState connection = ConnectionState::Idle;
    AudioProfile audio_profile = AudioProfile::None;
    bool has_current_device = false;
    Device current_device;
    std::vector<Device> nearby_devices;

    bool operator==(const ViewState& other) const {
        return mounted == other.mounted && enabled == other.enabled &&
               scanning == other.scanning && resetting == other.resetting &&
               connection == other.connection &&
               audio_profile == other.audio_profile &&
               has_current_device == other.has_current_device &&
               current_device == other.current_device &&
               nearby_devices == other.nearby_devices;
    }

    bool operator!=(const ViewState& other) const { return !(*this == other); }
};

}  // namespace agent_ui::bluetooth
