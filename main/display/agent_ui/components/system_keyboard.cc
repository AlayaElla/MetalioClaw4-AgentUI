#include "system_keyboard.h"

#include <array>
#include <string_view>

#include "fonts.h"
#include "haptic_feedback.h"
#include "render_snapshot_buffer.h"
#include "theme.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#endif

namespace agent_ui {

namespace {

constexpr lv_style_selector_t Selector(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

constexpr const char* kLowerMap[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    LV_SYMBOL_UP, "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "\n",
    "1#", "z", "x", "c", "v", "b", "n", "m", ".", "/", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", "123", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

constexpr const char* kUpperMap[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    LV_SYMBOL_UP, "A", "S", "D", "F", "G", "H", "J", "K", "L", "-", "\n",
    "1#", "Z", "X", "C", "V", "B", "N", "M", ".", "/", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", "123", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

constexpr const char* kSymbolMap[] = {
    // Keep the symbol page at five rows: four punctuation rows and the
    // shared footer.  The original letter-page 1# position remains the
    // first key in row four so it is a stable, one-tap return to letters.
    "!", "\"", "#", "$", "%", "&", "'", "(", ")", "*", LV_SYMBOL_BACKSPACE, "\n",
    "+", ",", "-", ".", "/", ":", ";", "<", "=", ">", "?", "\n",
    "@", "[", "\\", "]", "^", "_", "`", "{", "|", "}", "~", "\n",
    // The ten trailing shortcuts keep this row's 2+10 control-unit width
    // identical to the letter-page 1# row, so ABC never moves when switching.
    "ABC", "!", "\"", "#", "$", "%", "&", "'", "(", ")", "*", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", "123", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

// The address pad accepts IPv4 and a port directly; ABC also permits hostnames
// and full URLs without restricting the textarea's accepted characters.
constexpr const char* kAddressMap[] = {
    "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
    "4", "5", "6", ".", "\n",
    "7", "8", "9", ":", "\n",
    "1#", "0", "/", "-", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", "ABC", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

template <size_t N>
constexpr auto KeyControls(const char* const (&map)[N], bool address = false) {
    std::array<lv_buttonmatrix_ctrl_t, N> controls{};
    size_t index = 0;
    unsigned row = 0;
    for (const char* label : map) {
        const std::string_view key(label);
        if (key.empty()) continue;
        if (key == "\n") { ++row; continue; }
        unsigned width = key == " " ? 5 : 1;
        unsigned flags = 0;
        if (key == "ABC" || key == "1#" || key == "123" || key == LV_SYMBOL_UP ||
            key == LV_SYMBOL_KEYBOARD || key == LV_SYMBOL_OK) {
            width = 2;
            flags = LV_KEYBOARD_CTRL_BUTTON_FLAGS | LV_BUTTONMATRIX_CTRL_CHECKED;
        } else if (key == LV_SYMBOL_BACKSPACE || key == LV_SYMBOL_LEFT || key == LV_SYMBOL_RIGHT) {
            width = key == LV_SYMBOL_BACKSPACE ? 2 : 1;
            flags = LV_BUTTONMATRIX_CTRL_CHECKED;
        }
        controls[index++] = static_cast<lv_buttonmatrix_ctrl_t>(flags | (address && row < 4 ? 1 : width));
    }
    return controls;
}

constexpr auto kTextControls = KeyControls(kLowerMap);
constexpr auto kSymbolControls = KeyControls(kSymbolMap);
// The address grid has equal-width targets; every mode shares the same footer.
constexpr auto kAddressControls = KeyControls(kAddressMap, true);

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
    lv_obj_remove_style_all(keyboard_);
    lv_keyboard_set_map(keyboard_, LV_KEYBOARD_MODE_TEXT_LOWER, kLowerMap, kTextControls.data());
    lv_keyboard_set_map(keyboard_, LV_KEYBOARD_MODE_TEXT_UPPER, kUpperMap, kTextControls.data());
    lv_keyboard_set_map(keyboard_, LV_KEYBOARD_MODE_SPECIAL, kSymbolMap, kSymbolControls.data());
    lv_keyboard_set_map(keyboard_, LV_KEYBOARD_MODE_USER_1, kAddressMap, kAddressControls.data());
    lv_obj_remove_event_cb(keyboard_, lv_keyboard_def_event_cb);
    // Keep the key matrix at the verified on-screen position while its final
    // row remains fully inside the display.
    lv_obj_set_size(keyboard_, metrics::kDisplaySize, 312);
    lv_obj_set_pos(keyboard_, 0, 8);
    lv_obj_set_style_bg_opa(keyboard_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(keyboard_, LV_OPA_COVER, LV_PART_ITEMS);
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
    lv_obj_add_event_cb(keyboard_, RenderCacheCallback,
                        static_cast<lv_event_code_t>(LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS), this);
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
    if (mode == LV_KEYBOARD_MODE_USER_1) {
        lv_obj_add_flag(textarea, LV_OBJ_FLAG_USER_3);
    } else {
        lv_obj_remove_flag(textarea, LV_OBJ_FLAG_USER_3);
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
    RefreshRenderCache();
}

void Keyboard::Hide() {
    if (root_ == nullptr) return;
    lv_keyboard_set_textarea(keyboard_, nullptr);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    ReleaseRenderCache();
}

bool Keyboard::visible() const {
    return root_ != nullptr && !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void Keyboard::FocusCallback(lv_event_t* event) {
    auto* self = static_cast<Keyboard*>(lv_event_get_user_data(event));
    lv_obj_t* textarea = lv_event_get_target_obj(event);
    if (self == nullptr || textarea == nullptr) return;
    const lv_keyboard_mode_t mode = lv_obj_has_flag(textarea, LV_OBJ_FLAG_USER_3)
                                        ? LV_KEYBOARD_MODE_USER_1
                                        : lv_obj_has_flag(textarea, LV_OBJ_FLAG_USER_2)
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

void Keyboard::KeyPressedCallback(lv_event_t* event) {
    lv_obj_t* keyboard = lv_event_get_current_target_obj(event);
    const uint32_t selected = lv_keyboard_get_selected_button(keyboard);
    if (selected == LV_BUTTONMATRIX_BUTTON_NONE) return;
    auto* self = static_cast<Keyboard*>(lv_event_get_user_data(event));
    const lv_keyboard_mode_t old_mode = lv_keyboard_get_mode(keyboard);
    const char* text = lv_keyboard_get_button_text(keyboard, selected);
    if (text == nullptr) return;
    const std::string_view key(text);
    if (key == "123") {
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    } else if (key == "ABC") {
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    } else if (key == "1#") {
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_SPECIAL);
    } else if (key == LV_SYMBOL_UP) {
        lv_keyboard_set_mode(keyboard, lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER
                                           ? LV_KEYBOARD_MODE_TEXT_UPPER
                                           : LV_KEYBOARD_MODE_TEXT_LOWER);
    } else {
        lv_keyboard_def_event_cb(event);
    }
    if (self != nullptr && lv_keyboard_get_mode(keyboard) != old_mode) {
        self->RefreshRenderCache();
    }
    PlayHaptic(HapticStrength::Light);
}

void Keyboard::ApplyTheme() {
    if (root_ == nullptr || keyboard_ == nullptr) return;
    render_cache_ready_ = false;
    const auto& theme = Theme::Get();
    const auto& colors = theme.colors();
    const uint32_t background = colors.background;
    const uint32_t key = colors.surface;
    lv_obj_set_style_bg_color(root_, lv_color_hex(background), LV_PART_MAIN);
    lv_obj_set_style_border_color(root_, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(background), LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(key), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(colors.raised),
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

void Keyboard::RefreshRenderCache() {
#if LV_USE_SNAPSHOT
    render_cache_ready_ = false;
    render_snapshot_.Invalidate();
    if (keyboard_ == nullptr || !visible()) return;
    lv_obj_update_layout(keyboard_);
    const auto format = lv_display_get_color_format(lv_obj_get_display(keyboard_));
    const uint32_t width = lv_obj_get_width(keyboard_);
    const uint32_t height = lv_obj_get_height(keyboard_);
    if (!render_snapshot_.Prepare(width, height, format)) return;
    const size_t bytes = render_snapshot_.bytes();

    // Build outside DRAW_MAIN; the snapshot can dispatch draw tasks itself.
    // A mode switch may occur before the user's finger is released, so exclude
    // the temporary pressed state from the reusable image.
    taking_snapshot_ = true;
    const bool pressed = lv_obj_has_state(keyboard_, LV_STATE_PRESSED);
    if (pressed) lv_obj_remove_state(keyboard_, LV_STATE_PRESSED);
#ifdef ESP_PLATFORM
    const int64_t started_us = esp_timer_get_time();
#endif
    const bool captured = render_snapshot_.Capture(keyboard_);
    if (pressed) lv_obj_add_state(keyboard_, LV_STATE_PRESSED);
    taking_snapshot_ = false;
    // The keyboard has an opaque rectangular background and no external
    // effects. If that changes, keep native rendering rather than cache an
    // uninitialised border or crop an effect.
    render_cache_ready_ = captured;
    cached_mode_ = lv_keyboard_get_mode(keyboard_);
    if (render_cache_ready_) {
        render_snapshot_.RegisterKeyboard();
#ifdef ESP_PLATFORM
        ESP_LOGI("AgentKeyboard", "cache mode=%u bytes=%lu build=%lldus",
                 static_cast<unsigned>(cached_mode_), static_cast<unsigned long>(bytes),
                 static_cast<long long>(esp_timer_get_time() - started_us));
#endif
    }
#endif
}

void Keyboard::RenderCacheCallback(lv_event_t* event) {
    auto* self = static_cast<Keyboard*>(lv_event_get_user_data(event));
    if (self == nullptr || !self->render_cache_ready_ || self->taking_snapshot_) return;
    auto* keyboard = self->keyboard_;
    const lv_draw_buf_t* snapshot = self->render_snapshot_.buffer();
    // Use the native widget for transient touch and hardware-keyboard focus
    // feedback; its event handling and hit targets are never replaced.
    if (keyboard == nullptr || snapshot == nullptr ||
        lv_obj_has_state(keyboard, LV_STATE_PRESSED) ||
        lv_obj_has_state(keyboard, LV_STATE_FOCUS_KEY) ||
        lv_obj_has_state(keyboard, LV_STATE_EDITED) ||
        lv_keyboard_get_mode(keyboard) != self->cached_mode_) return;
    lv_area_t area;
    lv_obj_get_coords(keyboard, &area);
    if (lv_area_get_width(&area) != snapshot->header.w ||
        lv_area_get_height(&area) != snapshot->header.h) return;
    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.src = snapshot;
    lv_draw_image(lv_event_get_layer(event), &image, &area);
    lv_event_stop_processing(event);
}

void Keyboard::ReleaseRenderCache() {
    render_cache_ready_ = false;
    render_snapshot_.Release();
}

void Keyboard::RootDeletedCallback(lv_event_t* event) {
    auto* self = static_cast<Keyboard*>(lv_event_get_user_data(event));
    if (self == nullptr) return;
    self->ReleaseRenderCache();
    self->root_ = nullptr;
    self->keyboard_ = nullptr;
}

}  // namespace agent_ui
