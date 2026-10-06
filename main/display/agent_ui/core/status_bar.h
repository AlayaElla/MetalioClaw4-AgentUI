#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "agent_ui_types.h"
#include "components/expression_player.h"
#include "status_bar_data_provider.h"
#include "status_bar_state.h"
#include "theme.h"
#include "lvgl.h"

namespace agent_ui {

class StatusBar {
public:
    using AgentTapCallback = std::function<void()>;
    using BatteryUpdateCallback = std::function<void(bool, int, bool)>;

    static StatusBar& Get();

    void Initialize();
    void SetVisible(bool visible);
    void SetLockScreenMode(bool active);
    void SetHomeActive(bool active);
    void SetAgentState(AgentState state);
    void SetReplyCaption(const char* caption);
    void SetAgentTapCallback(AgentTapCallback callback);
    void SetBatteryUpdateCallback(BatteryUpdateCallback callback);
    void Refresh(bool force = false);
    void RefreshAsync();
    // Thread-safe invalidation; the next Refresh reads Settings on the GUI
    // owner thread and refreshes the cached network mode.
    void InvalidateNetworkModeCache() {
        data_provider_.InvalidateNetworkModeCache();
    }
    NetworkMode network_mode() const { return data_provider_.network_mode(); }

private:
    StatusBar() : data_provider_(GetSystemStatusBarDataSource()) {}
    static void TimerCallback(lv_timer_t* timer);
    static void DeletedCallback(lv_event_t* event);
    static void AgentFaceClicked(lv_event_t* event);
    static void SetAgentClusterTranslateY(void* object, int32_t value);
    static void SetObjectTranslateX(void* object, int32_t value);
    static void OnAgentClusterExitCompleted(lv_anim_t* animation);

    void Create();
    void AnimateAgentCluster(int32_t target, uint32_t duration_ms);
    void AnimateAgentFace(int32_t target, uint32_t duration_ms);
    void UpdateAgentPresentation();
    void SyncAgentExpressionRendering();
    void ClearReplyCaption();

    lv_obj_t* root_ = nullptr;
    lv_obj_t* left_cluster_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* agent_cluster_ = nullptr;
    lv_obj_t* agent_face_slot_ = nullptr;
    lv_obj_t* agent_dot_ = nullptr;
    lv_obj_t* agent_label_ = nullptr;
    lv_obj_t* reply_clip_ = nullptr;
    lv_obj_t* right_cluster_ = nullptr;
    lv_obj_t* bluetooth_icon_ = nullptr;
    lv_obj_t* network_icon_ = nullptr;
    lv_obj_t* battery_group_ = nullptr;
    lv_obj_t* battery_icon_ = nullptr;
    lv_obj_t* battery_outline_ = nullptr;
    std::array<lv_obj_t*, 3> battery_cells_{};
    lv_obj_t* battery_tip_ = nullptr;
    lv_obj_t* battery_bolt_outline_ = nullptr;
    lv_obj_t* battery_bolt_fill_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    lv_timer_t* reply_finish_timer_ = nullptr;
    StatusBarDataProvider data_provider_;
    AgentState agent_state_ = AgentState::Idle;
    std::unique_ptr<ExpressionPlayer> agent_expression_;
    AgentTapCallback agent_tap_callback_;
    BatteryUpdateCallback battery_update_callback_;
    std::string reply_caption_;
    std::string last_time_text_;
    std::string last_center_text_;
    std::string last_battery_text_;
    const lv_image_dsc_t* last_network_asset_ = nullptr;
    ThemeColors last_colors_{};
    uint32_t last_battery_outline_color_ = 0;
    int last_active_cells_ = -1;
    bool style_initialized_ = false;
    bool last_battery_charging_ = false;
    bool last_bluetooth_enabled_ = false;
    bool last_bluetooth_connected_ = false;
    bool visible_ = true;
    bool lock_screen_mode_ = false;
    bool home_active_ = true;
    bool ai_available_ = true;
    bool reply_wide_ = false;
    bool reply_presented_ = false;
    bool agent_expression_paused_ = false;
};

}  // namespace agent_ui
