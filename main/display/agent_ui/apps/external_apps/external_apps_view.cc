#include "external_apps_view.h"

#include <new>
#include <string>

#include "agent_ui/core/app_shell.h"
#include "agent_ui/core/fonts.h"
#include "agent_ui/core/theme.h"
#include "agent_ui/core/ui_utils.h"
#include "external_app_manager.h"
#include "external_app_runtime.h"

namespace agent_ui::external_apps {
namespace {

struct HostViewState {
    AppInfo app;
    lv_obj_t* root = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* actions = nullptr;
    lv_obj_t* launch_overlay = nullptr;
    lv_obj_t* launch_spinner = nullptr;
    lv_obj_t* launch_label = nullptr;
    lv_timer_t* launch_timer = nullptr;
    lv_timer_t* reveal_timer = nullptr;
    bool launch_started = false;
};

constexpr uint32_t kLaunchDelayMs = 50;
constexpr uint32_t kRevealDelayMs = 32;

lv_obj_t* CreateHint(lv_obj_t* parent, const char* text) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, fonts::Small(), LV_PART_MAIN);
    lv_obj_set_style_text_color(
        label, lv_color_hex(Theme::Get().colors().muted), LV_PART_MAIN);
    return label;
}

void DeleteTimer(lv_timer_t*& timer) {
    if (timer == nullptr) return;
    lv_timer_delete(timer);
    timer = nullptr;
}

void CreateLaunchOverlay(HostViewState* state) {
    if (state == nullptr || state->content == nullptr) return;
    const auto& colors = Theme::Get().colors();
    state->launch_overlay = lv_obj_create(state->content);
    lv_obj_remove_style_all(state->launch_overlay);
    lv_obj_set_size(state->launch_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(state->launch_overlay, 0, 0);
    lv_obj_set_style_bg_color(state->launch_overlay,
                              lv_color_hex(colors.background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->launch_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(state->launch_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(state->launch_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(state->launch_overlay, 24, LV_PART_MAIN);
    lv_obj_remove_flag(state->launch_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(state->launch_overlay, LV_OBJ_FLAG_CLICKABLE);

    state->launch_spinner = lv_spinner_create(state->launch_overlay);
    lv_obj_set_size(state->launch_spinner, 56, 56);
    lv_obj_set_style_arc_color(state->launch_spinner,
                               lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_arc_color(state->launch_spinner,
                               lv_color_hex(colors.accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(state->launch_spinner, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(state->launch_spinner, 7, LV_PART_INDICATOR);

    state->launch_label = lv_label_create(state->launch_overlay);
    const std::string message = "正在启动 " + state->app.name + "…";
    lv_label_set_text(state->launch_label, message.c_str());
    lv_obj_set_width(state->launch_label, 600);
    lv_label_set_long_mode(state->launch_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(state->launch_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(state->launch_label, fonts::MediumBold(),
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(state->launch_label,
                                lv_color_hex(colors.text), LV_PART_MAIN);
}

void RevealExternalApp(lv_timer_t* timer) {
    auto* state = static_cast<HostViewState*>(lv_timer_get_user_data(timer));
    if (state == nullptr) return;
    state->reveal_timer = nullptr;
    if (state->launch_overlay != nullptr &&
        lv_obj_is_valid(state->launch_overlay)) {
        lv_obj_add_flag(state->launch_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void LaunchExternalApp(lv_timer_t* timer) {
    auto* state = static_cast<HostViewState*>(lv_timer_get_user_data(timer));
    if (state == nullptr) return;
    state->launch_timer = nullptr;
    if (state->root == nullptr || !lv_obj_is_valid(state->root)) return;

    std::string launch_error;
    if (!Runtime::Get().Launch(state->app, state->content, state->actions,
                               &launch_error)) {
        if (state->launch_spinner != nullptr &&
            lv_obj_is_valid(state->launch_spinner)) {
            lv_obj_add_flag(state->launch_spinner, LV_OBJ_FLAG_HIDDEN);
        }
        if (state->launch_label != nullptr &&
            lv_obj_is_valid(state->launch_label)) {
            const std::string message = "启动失败：" + launch_error;
            lv_label_set_text(state->launch_label, message.c_str());
            lv_obj_set_style_text_color(state->launch_label,
                                        lv_color_hex(0xFF7D7D), LV_PART_MAIN);
        }
        return;
    }

    // The app has built its LVGL tree, but large image assets are decoded on
    // the next refresh. Keep this already-painted overlay above that first
    // refresh so the device never looks frozen while the decoder is busy.
    if (state->launch_label != nullptr &&
        lv_obj_is_valid(state->launch_label)) {
        lv_label_set_text(state->launch_label, "正在载入界面…");
    }
    if (state->launch_overlay != nullptr &&
        lv_obj_is_valid(state->launch_overlay)) {
        lv_obj_move_foreground(state->launch_overlay);
    }
    state->reveal_timer =
        lv_timer_create(RevealExternalApp, kRevealDelayMs, state);
    if (state->reveal_timer != nullptr) {
        lv_timer_set_repeat_count(state->reveal_timer, 1);
    } else if (state->launch_overlay != nullptr &&
               lv_obj_is_valid(state->launch_overlay)) {
        lv_obj_add_flag(state->launch_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void StartExternalAppAfterFirstFrame(lv_event_t* event) {
    auto* state =
        static_cast<HostViewState*>(lv_event_get_user_data(event));
    if (state == nullptr || state->launch_started) return;
    state->launch_started = true;
    state->launch_timer = lv_timer_create(LaunchExternalApp, kLaunchDelayMs,
                                          state);
    if (state->launch_timer != nullptr) {
        lv_timer_set_repeat_count(state->launch_timer, 1);
    }
}

void DeleteHostState(lv_event_t* event) {
    auto* state =
        static_cast<HostViewState*>(lv_event_get_user_data(event));
    if (state == nullptr) return;
    DeleteTimer(state->launch_timer);
    DeleteTimer(state->reveal_timer);
    Runtime::Get().Unload();
    delete state;
}

void OnLifecycle(AppLifecycleEvent event) {
    if (event == AppLifecycleEvent::Suspend) {
        Runtime::Get().SetPaused(true);
    } else if (event == AppLifecycleEvent::Resume ||
               event == AppLifecycleEvent::Load) {
        Runtime::Get().SetPaused(false);
    } else if (event == AppLifecycleEvent::Unload) {
        Runtime::Get().Unload();
    }
}

}  // namespace

lv_obj_t* HostView::Create() {
    const AppInfo* selected = Manager::Get().selected_app();
    auto shell = CreateAppShell(selected != nullptr ? selected->name.c_str() : "App",
                                nullptr);
    auto* state = new (std::nothrow) HostViewState();
    if (state == nullptr) return shell.root;
    state->root = shell.root;
    state->content = shell.content;
    state->actions = shell.actions;
    lv_obj_add_event_cb(shell.root, DeleteHostState, LV_EVENT_DELETE, state);
    AttachAppLifecycle(shell.root, OnLifecycle);

    if (selected == nullptr) {
        lv_obj_t* error = CreateHint(shell.content, "App 不可用，请返回主界面后重试。");
        lv_obj_set_pos(error, 28, 28);
        lv_obj_set_width(error, 664);
        return shell.root;
    }
    state->app = *selected;
    CreateLaunchOverlay(state);
    lv_obj_add_event_cb(shell.root, StartExternalAppAfterFirstFrame,
                        LV_EVENT_SCREEN_LOADED, state);
    return shell.root;
}

}  // namespace agent_ui::external_apps
