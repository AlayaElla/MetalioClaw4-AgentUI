#pragma once

#include <functional>

#include "core/ui_utils.h"
#include "lvgl.h"
#include "network_contract.h"
#include "network_dialogs_ui.h"
#include "network_settings_ui.h"
#include "network_view_state.h"

namespace agent_ui::network {

class View {
public:
    using IntentSink = std::function<void(const Intent&)>;

    void BuildInto(lv_obj_t* parent, IntentSink intent_sink);
    void Render(const ViewState& state);
    void Reset();
    void LifecycleCallback(Lifecycle lifecycle);

private:
    static void OnModeSelected(lv_event_t* event);
    static void OnScan(lv_event_t* event);
    static void OnInternalSelected(lv_event_t* event);
    static void OnExternalSelected(lv_event_t* event);
    static void OnSavedItem(lv_event_t* event);
    static void OnNearbyItem(lv_event_t* event);
    static void OnPasswordConnect(lv_event_t* event);
    static void OnFailureDismiss(lv_timer_t* timer);

    void Emit(const Intent& intent);
    void RenderLists(const ViewState& state);
    void RenderDialog(const ViewState& state);
    void CloseFailureTimer();

    IntentSink intent_sink_;
    lv_obj_t* screen_ = nullptr;
    network_settings_ui::Handles settings_{};
    network_dialogs_ui::View dialogs_{};
    ViewState state_{};
    std::string pending_ssid_;
    lv_timer_t* failure_timer_ = nullptr;
};

}  // namespace agent_ui::network
