#pragma once
#include "lvgl.h"
#include "components/ui_components.h"
#include "codex_menu_state.h"

namespace agent_ui::codex_menu_ui {
struct Callbacks {
    lv_event_cb_t dismiss = nullptr, tab = nullptr, task = nullptr, new_task = nullptr;
    lv_event_cb_t model = nullptr, effort = nullptr, fast = nullptr, ring = nullptr;
    lv_event_cb_t notification_enabled = nullptr;
    lv_event_cb_t mode = nullptr, connection_changed = nullptr, connect = nullptr;
};
struct Parts {
    int selected_tab = -1;
    std::string rendered_tabs[3];
    lv_obj_t *overlay = nullptr, *drawer = nullptr, *content = nullptr, *tab_bar = nullptr;
    lv_obj_t *panels[3]{}, *tabs[3]{};
    ui_components::StatusCardParts tasks[6]{};
    lv_obj_t *new_task = nullptr, *model_context = nullptr, *model_dropdown = nullptr;
    ui_components::ChoiceSliderParts effort{};
    lv_obj_t *fast_switch = nullptr, *fast_detail = nullptr;
    lv_obj_t *modes[2]{}, *connection_panels[2]{}, *discovery_name = nullptr;
    lv_obj_t *remote_ip = nullptr, *token = nullptr, *ring_switch = nullptr;
    lv_obj_t *notification_switch = nullptr;
    lv_obj_t *connection_status_text = nullptr, *connection_status_dot = nullptr;
    ui_components::ActionButtonParts connect{};
};
Parts Build(lv_obj_t* root, const Callbacks& callbacks);
void SelectTab(Parts& parts, int index);
void Refresh(Parts& parts, const codex_menu::State& state, bool pending, bool voice_busy, bool force = false);
void SetConnectionStatus(Parts& parts, bool connected, const char* text);
void SetNotificationSettings(Parts& parts, bool enabled);
}  // namespace agent_ui::codex_menu_ui
