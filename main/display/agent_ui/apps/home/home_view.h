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

private:
    IntentSink intent_sink_;
};

}  // namespace agent_ui::home
