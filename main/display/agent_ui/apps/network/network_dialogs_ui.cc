#include "network_dialogs_ui.h"

#include <cstdio>

#include <font_awesome.h>

#include "components/ui_components.h"
#include "components/system_keyboard.h"
#include "core/fonts.h"
#include "core/theme.h"
#include "core/ui_utils.h"
#include "i18n.h"

namespace agent_ui::network_dialogs_ui {
namespace {

constexpr int kStatusCardWidth = 596;
constexpr int kStatusCardHeight = 288;

lv_obj_t* AddLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                   uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    return label;
}

lv_obj_t* AddStatusIndicatorWell(lv_obj_t* card) {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* well = lv_obj_create(card);
    lv_obj_remove_style_all(well);
    lv_obj_set_size(well, 92, 92);
    lv_obj_set_pos(well, 30, 32);
    lv_obj_set_style_bg_color(well, lv_color_hex(colors.raised), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(well, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(well, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(well, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(well, metrics::kRadiusControl, LV_PART_MAIN);
    lv_obj_remove_flag(well, LV_OBJ_FLAG_SCROLLABLE);
    return well;
}

void AddStatusSpinner(lv_obj_t* well) {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* spinner = lv_spinner_create(well);
    lv_obj_set_size(spinner, 56, 56);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(colors.accent),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 7, LV_PART_INDICATOR);
    lv_obj_center(spinner);
}

void AddStatusHeading(lv_obj_t* card, const char* text, uint32_t color) {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* rule = lv_obj_create(card);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 58, 4);
    lv_obj_set_pos(rule, 150, 36);
    lv_obj_set_style_bg_color(rule, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* heading = AddLabel(card, text, fonts::MediumBold(), color);
    lv_obj_set_size(heading, 414, 82);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(heading, 150, 54);
}

lv_obj_t* AddStatusMessage(lv_obj_t* card, const char* text, uint32_t color) {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* divider = lv_obj_create(card);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 536, 1);
    lv_obj_set_pos(divider, 30, 207);
    lv_obj_set_style_bg_color(divider, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* message = AddLabel(card, text, fonts::Medium(), color);
    lv_obj_set_size(message, 536, 58);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_pos(message, 30, 224);
    return message;
}

}  // namespace

void View::Attach(lv_obj_t* screen) { screen_ = screen; }

void View::Reset() {
    screen_ = nullptr;
    password_overlay_ = nullptr;
    password_textarea_ = nullptr;
    status_overlay_ = nullptr;
    status_message_ = nullptr;
}

const char* View::Password() const {
    return password_textarea_ != nullptr ? lv_textarea_get_text(password_textarea_)
                                         : "";
}

lv_obj_t* View::CreateOverlay() {
    return ui_components::CreateModalOverlay(screen_);
}

lv_obj_t* View::CreateStatusCard() {
    status_overlay_ = CreateOverlay();
    if (status_overlay_ == nullptr) return nullptr;
    lv_obj_t* card = ui_components::CreateModalSurface(
        status_overlay_, kStatusCardWidth, kStatusCardHeight);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_center(card);
    return card;
}

void View::OpenPassword(const char* ssid, lv_event_cb_t connect_callback,
                        void* user_data) {
    ClosePassword();
    password_overlay_ = CreateOverlay();
    if (password_overlay_ == nullptr) return;
    const auto& colors = Theme::Get().colors();
    lv_obj_t* card = ui_components::CreateModalSurface(password_overlay_, 660, 300);
    lv_obj_set_pos(card, 30, 72);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);

    char title[128];
    std::snprintf(title, sizeof(title), I18n::T("连接到: %s"),
                  ssid != nullptr ? ssid : "");
    lv_obj_t* heading = AddLabel(card, title, fonts::MediumBold(), colors.text);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* hint = AddLabel(card, I18n::T("输入 WiFi 密码"), fonts::Medium(),
                              colors.muted);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 42);

    password_textarea_ = lv_textarea_create(card);
    lv_obj_set_size(password_textarea_, LV_PCT(100), 58);
    StyleTextInput(password_textarea_);
    lv_obj_align(password_textarea_, LV_ALIGN_TOP_LEFT, 0, 82);
    lv_textarea_set_one_line(password_textarea_, true);
    lv_textarea_set_password_mode(password_textarea_, true);
    IgnoreSwipeBack(password_textarea_, true);

    lv_obj_t* show = lv_checkbox_create(card);
    lv_checkbox_set_text(show, I18n::T("显示密码"));
    lv_obj_set_style_text_font(show, fonts::Medium(), LV_PART_MAIN);
    lv_obj_align(show, LV_ALIGN_BOTTOM_LEFT, 0, -6);
    lv_obj_add_event_cb(show, OnShowPassword, LV_EVENT_VALUE_CHANGED, this);
    IgnoreSwipeBack(show, true);

    lv_obj_t* cancel = ui_components::CreateButton(card);
    lv_obj_set_size(cancel, 120, 50);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(colors.raised), LV_PART_MAIN);
    lv_obj_set_style_radius(cancel, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, OnCancelPassword, LV_EVENT_CLICKED, this);
    lv_obj_t* cancel_label = AddLabel(cancel, I18n::T("取消"), fonts::Medium(),
                                      colors.text);
    lv_obj_center(cancel_label);

    lv_obj_t* connect = ui_components::CreateButton(card);
    lv_obj_set_size(connect, 120, 50);
    lv_obj_align(connect, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(connect, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_radius(connect, 12, LV_PART_MAIN);
    if (connect_callback != nullptr) {
        lv_obj_add_event_cb(connect, connect_callback, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t* connect_label = AddLabel(connect, I18n::T("连接"), fonts::Medium(),
                                       colors.accent_ink);
    lv_obj_center(connect_label);
    Keyboard::Get().Bind(password_textarea_, "Wi-Fi 密码");
    Keyboard::Get().Show(password_textarea_, "Wi-Fi 密码");
}

void View::OnShowPassword(lv_event_t* event) {
    auto* self = static_cast<View*>(lv_event_get_user_data(event));
    if (self == nullptr || self->password_textarea_ == nullptr) return;
    lv_textarea_set_password_mode(
        self->password_textarea_,
        !lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED));
}

void View::OnCancelPassword(lv_event_t* event) {
    auto* self = static_cast<View*>(lv_event_get_user_data(event));
    if (self != nullptr) self->ClosePassword();
}

void View::ClosePassword() {
    Keyboard::Get().Hide();
    if (password_overlay_ != nullptr) lv_obj_delete(password_overlay_);
    password_overlay_ = nullptr;
    password_textarea_ = nullptr;
}

void View::CloseStatus() {
    if (status_overlay_ != nullptr) lv_obj_delete(status_overlay_);
    status_overlay_ = nullptr;
    status_message_ = nullptr;
}

void View::OpenConnecting(const char* ssid) {
    CloseStatus();
    Keyboard::Get().Hide();
    lv_obj_t* card = CreateStatusCard();
    if (card == nullptr) return;
    const auto& colors = Theme::Get().colors();
    AddStatusSpinner(AddStatusIndicatorWell(card));
    AddStatusHeading(card, I18n::T("正在连接 Wi-Fi"), colors.text);
    status_message_ = AddStatusMessage(
        card, ssid != nullptr ? ssid : "", colors.muted);
}

void View::OpenFailure(const char* title, const char* detail) {
    CloseStatus();
    lv_obj_t* card = CreateStatusCard();
    if (card == nullptr) return;
    const auto& colors = Theme::Get().colors();
    lv_obj_t* well = AddStatusIndicatorWell(card);
    lv_obj_t* symbol = AddLabel(well, "!", fonts::Large(), colors.danger);
    lv_obj_center(symbol);
    AddStatusHeading(card, title, colors.danger);
    status_message_ = AddStatusMessage(card, detail, colors.text);
}

void View::OpenRestart(const char* headline, int seconds) {
    CloseStatus();
    lv_obj_t* card = CreateStatusCard();
    if (card == nullptr) return;
    const auto& colors = Theme::Get().colors();
    lv_obj_t* well = AddStatusIndicatorWell(card);
    lv_obj_t* check = AddLabel(well, FONT_AWESOME_CHECK, fonts::IconLarge(),
                               colors.accent);
    lv_obj_center(check);
    AddStatusHeading(card, headline, colors.text);
    status_message_ = AddStatusMessage(card, "", colors.muted);
    UpdateRestart(seconds);
}

void View::UpdateRestart(int seconds) {
    if (status_message_ == nullptr) return;
    char message[80];
    std::snprintf(message, sizeof(message), I18n::T("%d 秒后重启设备"), seconds);
    lv_label_set_text(status_message_, message);
}

void View::OpenNetworkSwitch(const char* target_name) {
    CloseStatus();
    lv_obj_t* card = CreateStatusCard();
    if (card == nullptr) return;
    const auto& colors = Theme::Get().colors();
    char headline[128];
    std::snprintf(headline, sizeof(headline), I18n::T("正在切换到 %s"),
                  target_name != nullptr ? target_name : "");
    AddStatusSpinner(AddStatusIndicatorWell(card));
    AddStatusHeading(card, headline, colors.text);
    status_message_ = AddStatusMessage(
        card, I18n::T("完成后设备将自动重启"), colors.muted);
}

void View::OpenSimSwitch(const char* slot_name) {
    CloseStatus();
    lv_obj_t* card = CreateStatusCard();
    if (card == nullptr) return;
    const auto& colors = Theme::Get().colors();
    AddStatusSpinner(AddStatusIndicatorWell(card));
    AddStatusHeading(card, I18n::T("正在切换 SIM 卡"), colors.text);
    char message[128];
    std::snprintf(message, sizeof(message), I18n::T("目标：%s"),
                  slot_name != nullptr ? slot_name : "");
    status_message_ = AddStatusMessage(card, message, colors.muted);
}

void View::SetStatusMessage(const char* text) {
    if (status_message_ != nullptr) {
        lv_label_set_text(status_message_, text != nullptr ? text : "");
    }
}

}  // namespace agent_ui::network_dialogs_ui
