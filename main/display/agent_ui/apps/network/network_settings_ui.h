#pragma once

#include <string>
#include <vector>

#include "lvgl.h"

namespace agent_ui::network_settings_ui {

struct SavedItem {
    std::string ssid;
    bool is_default = false;
};

struct NearbyItem {
    std::string ssid;
    std::string detail;
};

struct Model {
    bool cellular = false;
    bool external_slot = false;
};

struct Callbacks {
    void* owner = nullptr;
    lv_event_cb_t mode_selected = nullptr;
    lv_event_cb_t scan = nullptr;
    lv_event_cb_t internal_selected = nullptr;
    lv_event_cb_t external_selected = nullptr;
};

struct Handles {
    lv_obj_t* mode_buttons[3] = {};
    lv_obj_t* mode_panels[3] = {};
    lv_obj_t* saved_list = nullptr;
    lv_obj_t* saved_count = nullptr;
    lv_obj_t* nearby_list = nullptr;
    lv_obj_t* nearby_count = nullptr;
    lv_obj_t* nearby_spinner = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* scan_button = nullptr;
    lv_obj_t* scan_label = nullptr;
};

Handles Build(lv_obj_t* parent, const Model& model,
              const Callbacks& callbacks);
void ShowMode(Handles& handles, int selected);
void RenderSaved(Handles& handles, const std::vector<SavedItem>& items,
                 lv_event_cb_t callback);
void RenderNearby(Handles& handles, const std::vector<NearbyItem>& items,
                  bool scanning, bool scan_started, lv_event_cb_t callback);
void SetStatus(Handles& handles, const char* text, uint32_t color);
void SetScanEnabled(Handles& handles, bool enabled);
void SetSpinnerVisible(Handles& handles, bool visible);

}  // namespace agent_ui::network_settings_ui
