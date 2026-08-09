#pragma once

#include <atomic>

#include "apps/home/home_module.h"
#include "apps/camera/camera_module.h"
#include "core/agent_ui_types.h"
#include "core/global_ui_state.h"

class Board;

namespace agent_ui {

class Runtime {
public:
    static Runtime& Get();

    void Initialize();
    void OnBoardReady(Board& board);
    void Start();

    void SetAgentState(AgentState state);
    void SetConversationMessage(const char* role, const char* content);
    void SetSystemStatus(const char* status);
    void PlayDizzyExpression();

    const home::ViewState& home_state() const { return home_module_.state(); }
    const GlobalUiState& global_state() const { return global_state_; }

private:
    Runtime() = default;

    static lv_obj_t* CreateHomeView();
    static lv_obj_t* CreateCameraView();
    bool initialized_ = false;
    std::atomic<bool> board_ready_posted_{false};
    bool power_runtime_started_ = false;
    GlobalUiState global_state_;
    home::Module home_module_;
    camera::Module camera_module_;
};

}  // namespace agent_ui
