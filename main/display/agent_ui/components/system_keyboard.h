#pragma once

#include "lvgl.h"

namespace agent_ui {

class Keyboard {
public:
    static Keyboard& Get();

    void Initialize();
    void Bind(lv_obj_t* textarea, const char* field_name = nullptr,
              lv_keyboard_mode_t mode = LV_KEYBOARD_MODE_TEXT_LOWER);
    void Show(lv_obj_t* textarea, const char* field_name = nullptr,
              lv_keyboard_mode_t mode = LV_KEYBOARD_MODE_TEXT_LOWER);
    void Hide();
    bool visible() const;

private:
    Keyboard() = default;
    static void FocusCallback(lv_event_t* event);
    static void KeyboardCallback(lv_event_t* event);
    static void KeyPressedCallback(lv_event_t* event);
    static void RootDeletedCallback(lv_event_t* event);
    void ApplyTheme();

    lv_obj_t* root_ = nullptr;
    lv_obj_t* keyboard_ = nullptr;
};

}  // namespace agent_ui
