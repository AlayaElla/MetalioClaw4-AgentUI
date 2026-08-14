#include "system_keyboard.h"

#include "fonts.h"
#include "haptic_feedback.h"
#include "theme.h"

namespace agent_ui {

namespace {

constexpr lv_style_selector_t Selector(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

}  // namespace

Keyboard& Keyboard::Get() {
    static Keyboard instance;
    return instance;
}

void Keyboard::Initialize() {
    if (root_ != nullptr) return;
    root_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, metrics::kDisplaySize, 320);
    lv_obj_align(root_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(root_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(root_, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    keyboard_ = lv_keyboard_create(root_);
    // Keep the key matrix at the verified on-screen position while its final
    // row remains fully inside the display.
    lv_obj_set_size(keyboard_, metrics::kDisplaySize, 312);
    lv_obj_set_pos(keyboard_, 0, 8);
    lv_obj_set_style_bg_opa(keyboard_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_top(keyboard_, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(keyboard_, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(keyboard_, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(keyboard_, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_column(keyboard_, 5, LV_PART_MAIN);
    lv_obj_set_style_text_font(keyboard_, fonts::Keyboard(), LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboard_, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard_, 7, LV_PART_ITEMS);
    lv_obj_set_style_translate_y(
        keyboard_, 2, Selector(LV_PART_ITEMS, LV_STATE_PRESSED));
    lv_obj_add_event_cb(keyboard_, KeyPressedCallback, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(keyboard_, KeyboardCallback, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, KeyboardCallback, LV_EVENT_CANCEL, this);
    lv_obj_add_event_cb(root_, RootDeletedCallback, LV_EVENT_DELETE, this);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    ApplyTheme();
}

void Keyboard::Bind(lv_obj_t* textarea, const char*,
                    lv_keyboard_mode_t mode) {
    if (textarea == nullptr) return;
    if (mode == LV_KEYBOARD_MODE_NUMBER) {
        lv_obj_add_flag(textarea, LV_OBJ_FLAG_USER_2);
    } else {
        lv_obj_remove_flag(textarea, LV_OBJ_FLAG_USER_2);
    }
    lv_obj_add_event_cb(textarea, FocusCallback, LV_EVENT_FOCUSED, this);
}

void Keyboard::Show(lv_obj_t* textarea, const char*,
                    lv_keyboard_mode_t mode) {
    Initialize();
    if (textarea == nullptr) return;
    ApplyTheme();
    lv_keyboard_set_mode(keyboard_, mode);
    lv_keyboard_set_textarea(keyboard_, textarea);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
}

void Keyboard::Hide() {
    if (root_ == nullptr) return;
    lv_keyboard_set_textarea(keyboard_, nullptr);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

bool Keyboard::visible() const {
    return root_ != nullptr && !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void Keyboard::FocusCallback(lv_event_t* event) {
    auto* self = static_cast<Keyboard*>(lv_event_get_user_data(event));
    lv_obj_t* textarea = lv_event_get_target_obj(event);
    if (self == nullptr || textarea == nullptr) return;
    const lv_keyboard_mode_t mode = lv_obj_has_flag(textarea, LV_OBJ_FLAG_USER_2)
                                        ? LV_KEYBOARD_MODE_NUMBER
                                        : LV_KEYBOARD_MODE_TEXT_LOWER;
    self->Show(textarea, nullptr, mode);
}

void Keyboard::KeyboardCallback(lv_event_t* event) {
    auto* self = static_cast<Keyboard*>(lv_event_get_user_data(event));
    if (self == nullptr) return;
    if (lv_event_get_code(event) == LV_EVENT_READY && self->keyboard_ != nullptr) {
        lv_obj_t* textarea = lv_keyboard_get_textarea(self->keyboard_);
        if (textarea != nullptr) lv_obj_send_event(textarea, LV_EVENT_READY, nullptr);
    }
    self->Hide();
}

void Keyboard::KeyPressedCallback(lv_event_t*) {
    PlayHaptic(HapticStrength::Light);
}

void Keyboard::ApplyTheme() {
    if (root_ == nullptr || keyboard_ == nullptr) return;
    const auto& theme = Theme::Get();
    const auto& colors = theme.colors();
    const bool dark = theme.appearance_mode() == AppearanceMode::Dark;
    const uint32_t background = dark ? 0x080A09 : 0xE7E9E6;
    const uint32_t key = dark ? 0x202421 : 0xFFFFFF;
    lv_obj_set_style_bg_color(root_, lv_color_hex(background), LV_PART_MAIN);
    lv_obj_set_style_border_color(root_, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(background), LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(key), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(key),
                              Selector(LV_PART_ITEMS, LV_STATE_CHECKED));
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(colors.accent_pressed),
                              Selector(LV_PART_ITEMS, LV_STATE_PRESSED));
    lv_obj_set_style_bg_color(
        keyboard_, lv_color_hex(colors.accent_pressed),
        Selector(LV_PART_ITEMS,
                 static_cast<lv_state_t>(LV_STATE_CHECKED | LV_STATE_PRESSED)));
    lv_obj_set_style_text_color(keyboard_, lv_color_hex(colors.text), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard_, lv_color_hex(colors.text),
                                Selector(LV_PART_ITEMS, LV_STATE_CHECKED));
    lv_obj_set_style_text_color(keyboard_, lv_color_hex(colors.accent_ink),
                                Selector(LV_PART_ITEMS, LV_STATE_PRESSED));
    lv_obj_set_style_text_color(
        keyboard_, lv_color_hex(colors.accent_ink),
        Selector(LV_PART_ITEMS,
                 static_cast<lv_state_t>(LV_STATE_CHECKED | LV_STATE_PRESSED)));
    lv_obj_set_style_border_color(keyboard_, lv_color_hex(colors.border), LV_PART_ITEMS);
    lv_obj_set_style_border_color(keyboard_, lv_color_hex(colors.border),
                                  Selector(LV_PART_ITEMS, LV_STATE_CHECKED));
}

void Keyboard::RootDeletedCallback(lv_event_t* event) {
    auto* self = static_cast<Keyboard*>(lv_event_get_user_data(event));
    if (self == nullptr) return;
    self->root_ = nullptr;
    self->keyboard_ = nullptr;
}

}  // namespace agent_ui
