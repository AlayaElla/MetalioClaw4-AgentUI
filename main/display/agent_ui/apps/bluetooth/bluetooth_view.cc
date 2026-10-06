#include "bluetooth_view.h"

#include <cstdint>
#include <utility>

namespace agent_ui::bluetooth {

void View::BuildInto(lv_obj_t* parent, IntentSink intent_sink) {
    Reset();
    parent_ = parent;
    intent_sink_ = std::move(intent_sink);
    if (parent_ == nullptr) return;
    lv_obj_set_user_data(parent_, this);
    settings_ui_.Build(
        parent_,
        bluetooth_settings_ui::Model{
            .bluetooth_enabled = state_.enabled,
            .connected = state_.connection == ConnectionState::Connected,
            .profile_selected = state_.audio_profile != AudioProfile::None,
            .call_profile = state_.audio_profile == AudioProfile::Call,
        },
        bluetooth_settings_ui::Callbacks{
            .master_changed = OnEnabledChanged,
            .scan = OnScan,
            .music_profile = OnMusicProfile,
            .call_profile = OnCallProfile,
        });
    state_.mounted = true;
    Render(state_);
}

void View::Render(const ViewState& next) {
    if (parent_ == nullptr) {
        state_ = next;
        return;
    }
    settings_ui_.SetBluetoothEnabled(next.enabled);
    settings_ui_.SetScanning(next.scanning);
    settings_ui_.SetAudioProfile(
        next.connection == ConnectionState::Connected,
        next.audio_profile != AudioProfile::None,
        next.audio_profile == AudioProfile::Call);
    if (next.has_current_device != state_.has_current_device ||
        next.current_device != state_.current_device) {
        if (next.has_current_device) {
            settings_ui_.SetCurrentDevice(next.current_device.address.c_str(),
                                          next.current_device.name.c_str());
        } else {
            settings_ui_.SetCurrentDevice(nullptr, nullptr);
        }
    }
    if (next.nearby_devices != state_.nearby_devices) {
        RenderDevices(next);
    }
    if (next.connection != ConnectionState::Connecting) {
        settings_ui_.ClearConnectingDevice();
    }
    state_ = next;
}

void View::RenderDevices(const ViewState& state) {
    settings_ui_.ClearDevices();
    for (const Device& device : state.nearby_devices) {
        settings_ui_.AddDevice(
            device.address.c_str(), device.name.c_str(), OnDevice, nullptr);
    }
    settings_ui_.FinishDeviceUpdate();
}

void View::Reset() {
    if (parent_ != nullptr && lv_obj_is_valid(parent_)) {
        lv_obj_set_user_data(parent_, nullptr);
    }
    settings_ui_.Reset();
    intent_sink_ = nullptr;
    parent_ = nullptr;
    state_ = {};
}

void View::LifecycleCallback(Lifecycle lifecycle) {
    if (lifecycle == Lifecycle::Unload) Reset();
}

View* View::OwnerFromEvent(lv_event_t* event) {
    lv_obj_t* object = lv_event_get_current_target_obj(event);
    while (object != nullptr) {
        if (void* owner = lv_obj_get_user_data(object)) {
            return static_cast<View*>(owner);
        }
        object = lv_obj_get_parent(object);
    }
    return nullptr;
}

void View::Emit(const Intent& intent) {
    if (intent_sink_) intent_sink_(intent);
}

void View::OnEnabledChanged(lv_event_t* event) {
    View* self = OwnerFromEvent(event);
    if (self == nullptr) return;
    lv_obj_t* control = lv_event_get_target_obj(event);
    self->Emit(Intent::SetEnabled(
        lv_obj_has_state(control, LV_STATE_CHECKED)));
}

void View::OnScan(lv_event_t* event) {
    if (View* self = OwnerFromEvent(event)) self->Emit(Intent::Scan());
}

void View::OnMusicProfile(lv_event_t* event) {
    if (View* self = OwnerFromEvent(event)) {
        self->Emit(Intent::SetAudioProfile(AudioProfile::Music));
    }
}

void View::OnCallProfile(lv_event_t* event) {
    if (View* self = OwnerFromEvent(event)) {
        self->Emit(Intent::SetAudioProfile(AudioProfile::Call));
    }
}

void View::OnDevice(lv_event_t* event) {
    View* self = OwnerFromEvent(event);
    if (self == nullptr) return;
    const std::size_t index = self->settings_ui_.DeviceIndex(
        lv_event_get_current_target_obj(event));
    if (index >= self->state_.nearby_devices.size()) return;
    self->settings_ui_.SetConnectingDevice(index);
    self->Emit(Intent::Connect(index));
}

}  // namespace agent_ui::bluetooth
