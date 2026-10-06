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
    static void RenderCacheCallback(lv_event_t* event);
    static void RootDeletedCallback(lv_event_t* event);
    void ApplyTheme();
    void RefreshRenderCache();
    void ReleaseRenderCache();

    lv_obj_t* root_ = nullptr;
    lv_obj_t* keyboard_ = nullptr;
    lv_draw_buf_t render_cache_{};
    void* render_cache_memory_ = nullptr;
    bool render_cache_ready_ = false;
    bool taking_snapshot_ = false;
    lv_keyboard_mode_t cached_mode_ = LV_KEYBOARD_MODE_TEXT_LOWER;
};

}  // namespace agent_ui
