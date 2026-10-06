#pragma once

#include <functional>

#include "core/app_module.h"
#include "home_adapter.h"
#include "home_controller.h"
#include "home_view.h"

namespace agent_ui::home {

class Module final : public AppModule {
public:
    using NavigationSink = std::function<void(ScreenId)>;

    void Initialize(const char* initial_message, NavigationSink navigation_sink);
    void HandleEvent(const Event& event);
    void HandleIntent(const Intent& intent) { controller_.HandleIntent(intent); }

    ScreenId screen_id() const override { return ScreenId::Home; }
    lv_obj_t* Mount() override;
    void Unmount() override;

    const ViewState& state() const { return controller_.state(); }
    void NotifyUserActivity() { view_.NotifyUserActivity(); }
    void SleepExpression() { view_.SleepExpression(); }
    bool IsMounted() const { return view_.IsMounted(); }
    void UpdateBattery(bool has_battery, int level, bool charging) {
        view_.UpdateBattery(has_battery, level, charging);
    }
    void PlayDizzy() { view_.PlayDizzy(); }
    void HoldChargingExpression() { view_.HoldChargingExpression(); }
    void HoldDizzyExpression() { view_.HoldDizzyExpression(); }
    void ReleaseSpecialExpression() { view_.ReleaseSpecialExpression(); }
    bool OwnsScreen(lv_obj_t* screen) const { return view_.OwnsScreen(screen); }
    void SetRenderingPaused(bool paused) { view_.SetRenderingPaused(paused); }

private:
    void HandleCommand(const Command& command);

    NavigationSink navigation_sink_;
    Controller controller_;
    View view_;
    Adapter adapter_;
};

}  // namespace agent_ui::home
