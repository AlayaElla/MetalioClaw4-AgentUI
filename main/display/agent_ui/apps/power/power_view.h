#pragma once

namespace agent_ui {

class PowerView {
public:
    static void ShowDialog();
    static void BeginShutdown(const char* reason = nullptr);
};

}  // namespace agent_ui
