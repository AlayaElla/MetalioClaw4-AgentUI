#include "boot_view.h"

#include "components/expression_player.h"
#include "core/fonts.h"
#include "core/theme.h"

namespace agent_ui {
namespace {

void DeleteExpressionPlayer(lv_event_t* event) {
    auto* player =
        static_cast<ExpressionPlayer*>(lv_event_get_user_data(event));
    delete player;
}

}  // namespace

lv_obj_t* BootView::Create() {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* screen = lv_obj_create(nullptr);
    StyleRoot(screen);

    lv_obj_t* expression = lv_obj_create(screen);
    lv_obj_remove_style_all(expression);
    lv_obj_set_size(expression, 600, 400);
    lv_obj_set_pos(expression, 60, 110);
    lv_obj_remove_flag(expression, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(expression, LV_OBJ_FLAG_CLICKABLE);
    auto* expression_player = new ExpressionPlayer(expression);
    expression_player->PlayBootAnimation();
    lv_obj_add_event_cb(screen, DeleteExpressionPlayer, LV_EVENT_DELETE,
                        expression_player);

    lv_obj_t* copy = lv_obj_create(screen);
    lv_obj_remove_style_all(copy);
    lv_obj_set_size(copy, 636, 183);
    lv_obj_align(copy, LV_ALIGN_BOTTOM_LEFT, metrics::kSystemPadding, -34);
    lv_obj_remove_flag(copy, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* rule = lv_obj_create(copy);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 58, 4);
    lv_obj_set_style_bg_color(rule, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(rule, 0, 0);

    lv_obj_t* title = lv_label_create(copy);
    lv_label_set_text(title, "正在启动");
    lv_obj_set_style_text_font(title, fonts::LargeBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_align_to(title, rule, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 22);

    lv_obj_t* detail = lv_label_create(copy);
    lv_label_set_text(detail, "初始化设备服务");
    lv_obj_set_style_text_font(detail, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(detail, lv_color_hex(colors.muted), LV_PART_MAIN);
    lv_obj_align_to(detail, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);

    lv_obj_t* track = lv_obj_create(copy);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 636, 12);
    lv_obj_set_style_bg_color(track, lv_color_hex(colors.raised), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(track, 0, LV_PART_MAIN);
    lv_obj_align_to(track, detail, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 24);

    lv_obj_t* progress = lv_obj_create(track);
    lv_obj_remove_style_all(progress);
    lv_obj_set_size(progress, 458, 12);
    lv_obj_set_style_bg_color(progress, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(progress, 0, LV_PART_MAIN);
    lv_obj_align(progress, LV_ALIGN_LEFT_MID, 0, 0);

    return screen;
}

}  // namespace agent_ui
