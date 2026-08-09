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

    ScreenId screen_id() const override { return ScreenId::Home; }
    lv_obj_t* Mount() override;
    void Unmount() override;

    const ViewState& state() const { return controller_.state(); }

private:
    void HandleCommand(const Command& command);

    NavigationSink navigation_sink_;
    Controller controller_;
    View view_;
    Adapter adapter_;
};

}  // namespace agent_ui::home
