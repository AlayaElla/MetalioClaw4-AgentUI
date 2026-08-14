#pragma once

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace agent_ui::settings_panels_ui {

struct GeneralModel {
    size_t appearance = 0;
    size_t accent = 0;
    int brightness_min = 0;
    int brightness = 0;
    int volume = 0;
    int standby_index = 1;
};

struct GeneralCallbacks {
    lv_event_cb_t appearance = nullptr;
    lv_event_cb_t accent = nullptr;
    lv_event_cb_t brightness = nullptr;
    lv_event_cb_t volume = nullptr;
    lv_event_cb_t standby = nullptr;
};

struct GeneralHandles {
    lv_obj_t* brightness_value = nullptr;
    lv_obj_t* volume_value = nullptr;
    lv_obj_t* standby_value = nullptr;
};

struct AiModel {
    bool wake_enabled = true;
    bool hermes_selected = false;
    const char* hermes_dashboard_url = nullptr;
    const char* hermes_username = nullptr;
    bool hermes_password_configured = false;
    const char* hermes_profile = nullptr;
    const char* hermes_test_status = nullptr;
    const char* hermes_apply_status = nullptr;
};

struct AiCallbacks {
    lv_event_cb_t wake_changed = nullptr;
    lv_event_cb_t provider_changed = nullptr;
    lv_event_cb_t hermes_field_committed = nullptr;
    lv_event_cb_t hermes_save = nullptr;
    lv_event_cb_t hermes_test = nullptr;
};

struct AiHandles {
    lv_obj_t* hermes_dashboard_url = nullptr;
    lv_obj_t* hermes_username = nullptr;
    lv_obj_t* hermes_password = nullptr;
    lv_obj_t* hermes_profile = nullptr;
    lv_obj_t* hermes_test_status = nullptr;
    lv_obj_t* hermes_apply_status = nullptr;
};

struct LanguageOption {
    const char* label = nullptr;
    uintptr_t id = 0;
    bool selected = false;
};

struct AboutInfo {
    const char* product = nullptr;
    const char* version = nullptr;
    const char* device = nullptr;
    const char* display = nullptr;
};

GeneralHandles BuildGeneral(lv_obj_t* parent, const GeneralModel& model,
                            const GeneralCallbacks& callbacks);
AiHandles BuildAi(lv_obj_t* parent, const AiModel& model,
                  const AiCallbacks& callbacks);
void BuildLanguage(lv_obj_t* parent, const LanguageOption* options,
                   size_t option_count, lv_event_cb_t callback);
void BuildAbout(lv_obj_t* parent, const AboutInfo& info);

}  // namespace agent_ui::settings_panels_ui
