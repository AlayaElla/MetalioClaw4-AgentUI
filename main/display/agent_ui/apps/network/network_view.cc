#include "network_view.h"

#include <cstdint>
#include <utility>

#include "network_settings_ui.h"

namespace agent_ui::network {
namespace {

View* OwnerFromEvent(lv_event_t* event) {
    lv_obj_t* object = lv_event_get_current_target_obj(event);
    while (object != nullptr) {
        if (void* owner = lv_obj_get_user_data(object)) {
            return static_cast<View*>(owner);
        }
        object = lv_obj_get_parent(object);
    }
    return nullptr;
}

}  // namespace

void View::BuildInto(lv_obj_t* parent, IntentSink intent_sink) {
    Reset();
    intent_sink_ = std::move(intent_sink);
    screen_ = parent != nullptr ? lv_obj_get_screen(parent) : nullptr;
    if (screen_ == nullptr) return;

    dialogs_.Attach(screen_);
    settings_ = network_settings_ui::Build(
        parent,
        network_settings_ui::Model{
            .cellular = state_.cellular,
            .external_slot = state_.external_slot,
        },
        network_settings_ui::Callbacks{
            .owner = this,
            .mode_selected = OnModeSelected,
            .scan = OnScan,
            .internal_selected = OnInternalSelected,
            .external_selected = OnExternalSelected,
        });
    state_.mounted = true;
    Render(state_);
}

void View::Emit(const Intent& intent) {
    if (intent_sink_) intent_sink_(intent);
}

void View::RenderLists(const ViewState& state) {
    if (settings_.saved_list == nullptr) return;

    std::vector<network_settings_ui::SavedItem> saved;
    saved.reserve(state.saved_networks.size());
    for (const auto& item : state.saved_networks) {
        saved.push_back({item.ssid, item.is_default});
    }
    network_settings_ui::RenderSaved(settings_, saved, OnSavedItem);

    std::vector<network_settings_ui::NearbyItem> nearby;
    nearby.reserve(state.nearby_networks.size());
    for (const auto& item : state.nearby_networks) {
        nearby.push_back({item.ssid, item.detail});
    }
    network_settings_ui::RenderNearby(
        settings_, nearby, state.scanning, state.scan_started, OnNearbyItem);
    network_settings_ui::ShowMode(settings_, state.selected_mode);
    network_settings_ui::SetStatus(
        settings_, state.status.c_str(), state.status_color);
    network_settings_ui::SetScanEnabled(settings_, !state.scanning);
    network_settings_ui::SetSpinnerVisible(settings_, state.scanning);
}

void View::CloseFailureTimer() {
    if (failure_timer_ != nullptr) {
        lv_timer_delete(failure_timer_);
        failure_timer_ = nullptr;
    }
}

void View::RenderDialog(const ViewState& next) {
    const Dialog previous = state_.dialog;
    if (previous != next.dialog) {
        CloseFailureTimer();
        if (previous == Dialog::Password && next.dialog != Dialog::Connecting) {
            dialogs_.ClosePassword();
        }
        if (previous != Dialog::None && previous != Dialog::Password) {
            dialogs_.CloseStatus();
        }

        switch (next.dialog) {
            case Dialog::None:
                dialogs_.ClosePassword();
                dialogs_.CloseStatus();
                break;
            case Dialog::Password:
                pending_ssid_ = next.dialog_ssid;
                dialogs_.OpenPassword(next.dialog_ssid.c_str(), OnPasswordConnect,
                                      this);
                break;
            case Dialog::Connecting:
                dialogs_.OpenConnecting(next.dialog_ssid.c_str());
                break;
            case Dialog::Failure:
                dialogs_.OpenFailure(next.dialog_title.c_str(),
                                     next.dialog_detail.c_str());
                if (next.failure_auto_close_ms > 0) {
                    failure_timer_ = lv_timer_create(OnFailureDismiss,
                                                      next.failure_auto_close_ms,
                                                      this);
                    lv_timer_set_repeat_count(failure_timer_, 1);
                }
                break;
            case Dialog::Restart:
                dialogs_.ClosePassword();
                dialogs_.OpenRestart(next.dialog_title.c_str(),
                                     next.restart_remaining);
                break;
            case Dialog::NetworkSwitch:
                dialogs_.OpenNetworkSwitch(next.dialog_title.c_str());
                break;
            case Dialog::SimSwitch:
                dialogs_.OpenSimSwitch(next.dialog_title.c_str());
                break;
        }
    } else if (next.dialog == Dialog::Restart) {
        dialogs_.UpdateRestart(next.restart_remaining);
    }
}

void View::Render(const ViewState& next) {
    RenderLists(next);
    RenderDialog(next);
    state_ = next;
}

void View::Reset() {
    CloseFailureTimer();
    dialogs_.Reset();
    settings_ = {};
    screen_ = nullptr;
    pending_ssid_.clear();
    intent_sink_ = nullptr;
    state_ = {};
}

void View::LifecycleCallback(Lifecycle lifecycle) {
    if (lifecycle == Lifecycle::Unload) Reset();
}

void View::OnModeSelected(lv_event_t* event) {
    View* self = OwnerFromEvent(event);
    if (self == nullptr) return;
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(event)));
    self->Emit(Intent::SelectMode(mode));
}

void View::OnScan(lv_event_t* event) {
    if (View* self = OwnerFromEvent(event)) self->Emit(Intent::Scan());
}

void View::OnInternalSelected(lv_event_t* event) {
    if (View* self = OwnerFromEvent(event)) {
        self->Emit(Intent::SelectSimSlot(1));
    }
}

void View::OnExternalSelected(lv_event_t* event) {
    if (View* self = OwnerFromEvent(event)) {
        self->Emit(Intent::SelectSimSlot(0));
    }
}

void View::OnSavedItem(lv_event_t* event) {
    View* self = OwnerFromEvent(event);
    if (self == nullptr) return;
    const std::size_t encoded = reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event));
    if (encoded == 0) return;
    self->Emit(Intent::ConnectSaved(encoded - 1));
}

void View::OnNearbyItem(lv_event_t* event) {
    View* self = OwnerFromEvent(event);
    if (self == nullptr) return;
    const std::size_t encoded = reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event));
    if (encoded == 0 || encoded > self->state_.nearby_networks.size()) return;
    const auto& item = self->state_.nearby_networks[encoded - 1];
    self->Emit(Intent::RequestPassword(encoded - 1, item.ssid));
}

void View::OnPasswordConnect(lv_event_t* event) {
    auto* self = static_cast<View*>(lv_event_get_user_data(event));
    if (self == nullptr) return;
    const char* password = self->dialogs_.Password();
    self->Emit(Intent::SubmitPassword(
        self->pending_ssid_, password != nullptr ? password : ""));
}

void View::OnFailureDismiss(lv_timer_t* timer) {
    auto* self = static_cast<View*>(lv_timer_get_user_data(timer));
    if (self == nullptr) return;
    self->failure_timer_ = nullptr;
    self->Emit(Intent::DismissStatus());
}

}  // namespace agent_ui::network
