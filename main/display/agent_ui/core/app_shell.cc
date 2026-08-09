#include "app_shell.h"

#include <font_awesome.h>

#include "components/ui_components.h"
#include "navigation.h"
#include "theme.h"

namespace agent_ui {
void OnBack(lv_event_t*) { Navigation::Get().Back(); }

AppShell CreateAppShell(const char* title, const char* subtitle, bool show_back,
                        lv_event_cb_t back_callback) {
    AppShell shell;
    shell.root = lv_obj_create(nullptr);
    StyleRoot(shell.root);
    (void)title;
    (void)subtitle;

    shell.content = lv_obj_create(shell.root);
    lv_obj_remove_style_all(shell.content);
    lv_obj_set_size(shell.content, metrics::kDisplaySize,
                    metrics::kBottomActionContentHeight);
    lv_obj_set_pos(shell.content, 0, metrics::kStatusBarHeight);
    lv_obj_set_style_bg_color(shell.content,
                              lv_color_hex(Theme::Get().colors().background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(shell.content, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(shell.content, LV_OBJ_FLAG_SCROLLABLE);

    shell.actions = ui_components::CreateBottomActionBar(shell.root);
    if (show_back) {
        shell.back = ui_components::AddBottomActionButton(
            shell.actions, FONT_AWESOME_ARROW_LEFT, "返回",
            back_callback != nullptr ? back_callback : OnBack).root;
    }
    return shell;
}

}  // namespace agent_ui
