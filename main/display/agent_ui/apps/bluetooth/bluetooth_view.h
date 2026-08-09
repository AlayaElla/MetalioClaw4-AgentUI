#pragma once

#include <functional>

#include "bluetooth_contract.h"
#include "bluetooth_settings_ui.h"
#include "bluetooth_view_state.h"
#include "lvgl.h"

namespace agent_ui::bluetooth {

class View {
public:
    using IntentSink = std::function<void(const Intent&)>;

    void BuildInto(lv_obj_t* parent, IntentSink intent_sink);
    void Render(const ViewState& state);
    void Reset();
    void LifecycleCallback(Lifecycle lifecycle);

private:
    static View* OwnerFromEvent(lv_event_t* event);
    static void OnEnabledChanged(lv_event_t* event);
    static void OnScan(lv_event_t* event);
    static void OnMusicProfile(lv_event_t* event);
    static void OnCallProfile(lv_event_t* event);
    static void OnDevice(lv_event_t* event);

    void Emit(const Intent& intent);
    void RenderDevices(const ViewState& state);

    IntentSink intent_sink_;
    bluetooth_settings_ui::View settings_ui_;
    ViewState state_;
    lv_obj_t* parent_ = nullptr;
};

}  // namespace agent_ui::bluetooth
