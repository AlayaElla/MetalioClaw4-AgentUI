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
};

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

void DeleteHostState(lv_event_t* event) {
    Runtime::Get().Unload();
    delete static_cast<HostViewState*>(lv_event_get_user_data(event));
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
    lv_obj_add_event_cb(shell.root, DeleteHostState, LV_EVENT_DELETE, state);
    AttachAppLifecycle(shell.root, OnLifecycle);

    if (selected == nullptr) {
        lv_obj_t* error = CreateHint(shell.content, "App 不可用，请返回主界面后重试。");
        lv_obj_set_pos(error, 28, 28);
        lv_obj_set_width(error, 664);
        return shell.root;
    }
    state->app = *selected;

    std::string launch_error;
    if (!Runtime::Get().Launch(state->app, shell.content, shell.actions,
                               &launch_error)) {
        const std::string message = "启动失败：" + launch_error;
        lv_obj_t* error = CreateHint(shell.content, message.c_str());
        lv_obj_set_pos(error, 28, 28);
        lv_obj_set_width(error, 664);
    }
    return shell.root;
}

}  // namespace agent_ui::external_apps
