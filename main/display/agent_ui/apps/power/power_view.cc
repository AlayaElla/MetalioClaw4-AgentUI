#include "power_view.h"

#include <esp_log.h>
#include <font_awesome.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "IOExpander.hpp"
#include "application.h"
#include "components/ui_components.h"
#include "core/fonts.h"
#include "core/status_bar.h"
#include "core/theme.h"

namespace agent_ui {
namespace {

constexpr char kTag[] = "AgentPower";
lv_obj_t* s_dialog = nullptr;
bool s_shutting_down = false;

void CloseDialog() {
    if (s_dialog != nullptr) lv_obj_delete(s_dialog);
    s_dialog = nullptr;
}

void ShutdownPulseTask(void*) {
    auto& io = IOExpander::getInstance();
    for (;;) {
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, true);
        vTaskDelay(pdMS_TO_TICKS(100));
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void OnClose(lv_event_t*) { CloseDialog(); }
void OnRestart(lv_event_t*) {
    CloseDialog();
    Application::GetInstance().Reboot();
}
void OnShutdown(lv_event_t*) { PowerView::BeginShutdown("User confirmed shutdown"); }

lv_obj_t* CreateAction(lv_obj_t* parent, const char* text, uint32_t background,
                       uint32_t border, uint32_t text_color, int y,
                       lv_event_cb_t callback) {
    lv_obj_t* button = ui_components::CreateButton(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 596, 92);
    lv_obj_set_pos(button, 62, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, background == 0 ? LV_OPA_TRANSP : LV_OPA_COVER,
                            LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* text_label = lv_label_create(button);
    lv_label_set_text(text_label, text);
    lv_obj_set_style_text_font(text_label, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(text_label, lv_color_hex(text_color), LV_PART_MAIN);
    return button;
}

}  // namespace

void PowerView::ShowDialog() {
    if (s_dialog != nullptr || s_shutting_down) return;
    const auto& colors = Theme::Get().colors();
    s_dialog = lv_obj_create(lv_layer_top());
    StyleRoot(s_dialog);

    lv_obj_t* symbol = lv_label_create(s_dialog);
    lv_label_set_text(symbol, FONT_AWESOME_POWER_OFF);
    lv_obj_set_style_text_font(symbol, fonts::IconLarge(), LV_PART_MAIN);
    lv_obj_set_style_text_color(symbol, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_align(symbol, LV_ALIGN_TOP_MID, 0, 66);

    lv_obj_t* title = lv_label_create(s_dialog);
    lv_label_set_text(title, "电源选项");
    lv_obj_set_style_text_font(title, fonts::Large(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 132);

    lv_obj_t* hint = lv_label_create(s_dialog);
    lv_label_set_text(hint, "选择要执行的操作");
    lv_obj_set_style_text_font(hint, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(colors.muted), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 198);

    CreateAction(s_dialog, "取消", 0, colors.text, colors.text, 278, OnClose);
    CreateAction(s_dialog, "重新启动", colors.accent, colors.accent,
                 colors.accent_ink, 384, OnRestart);
    CreateAction(s_dialog, "关机", colors.danger, colors.danger, 0xFFFFFF, 490,
                 OnShutdown);
}

void PowerView::BeginShutdown(const char* reason) {
    if (s_shutting_down) return;
    s_shutting_down = true;
    ESP_LOGW(kTag, "Shutdown started: %s", reason != nullptr ? reason : "system");
    CloseDialog();

    lv_obj_t* screen = lv_obj_create(nullptr);
    StyleRoot(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x030403), LV_PART_MAIN);
    lv_obj_t* icon = lv_label_create(screen);
    lv_label_set_text(icon, FONT_AWESOME_POWER_OFF);
    lv_obj_set_style_text_font(icon, fonts::Icon(), LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(Theme::Get().colors().accent),
                                LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -42);
    lv_obj_t* label = lv_label_create(screen);
    lv_label_set_text(label, "正在关机...");
    lv_obj_set_style_text_font(label, fonts::Large(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(Theme::Get().colors().text),
                                LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 42);
    lv_screen_load(screen);
    xTaskCreate(ShutdownPulseTask, "agent_power_off", 2048, nullptr, 5, nullptr);
}

}  // namespace agent_ui
