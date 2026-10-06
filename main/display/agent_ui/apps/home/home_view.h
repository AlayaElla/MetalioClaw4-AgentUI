#pragma once

#include <functional>

#include "home_contract.h"
#include "home_view_state.h"
#include "lvgl.h"

namespace agent_ui::home {

class View {
public:
    using IntentSink = std::function<void(const Intent&)>;

    lv_obj_t* Mount(IntentSink intent_sink);
    void Render(const ViewState& state);
    void Unmount();
    void NotifyUserActivity();
    void SleepExpression();
    bool IsMounted() const;
    void UpdateBattery(bool has_battery, int level, bool charging);
    void PlayDizzy();
    void HoldChargingExpression();
    void HoldDizzyExpression();
    void ReleaseSpecialExpression();
    bool OwnsScreen(lv_obj_t* screen) const;
    void SetRenderingPaused(bool paused);

private:
    IntentSink intent_sink_;
};

}  // namespace agent_ui::home
