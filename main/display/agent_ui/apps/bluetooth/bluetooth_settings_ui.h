#pragma once

#include <cstddef>
#include <vector>

#include "lvgl.h"

namespace agent_ui::bluetooth_settings_ui {

struct Model {
    bool bluetooth_enabled = false;
    bool connected = false;
    bool profile_selected = false;
    bool call_profile = false;
};

struct Callbacks {
    lv_event_cb_t master_changed = nullptr;
    lv_event_cb_t scan = nullptr;
    lv_event_cb_t music_profile = nullptr;
    lv_event_cb_t call_profile = nullptr;
};

class View {
public:
    void Build(lv_obj_t* parent, const Model& model,
               const Callbacks& callbacks);
    void Reset();
    void SetBluetoothEnabled(bool enabled);
    void SetScanning(bool scanning);
    void SetAudioProfile(bool connected, bool selected, bool call_profile);
    void SetConnectingDevice(std::size_t index);
    void ClearConnectingDevice();
    void ClearDevices();
    void SetCurrentDevice(const char* address, const char* name);
    lv_obj_t* AddDevice(const char* address, const char* name,
                        lv_event_cb_t callback, void* user_data);

private:
    lv_obj_t* master_status_ = nullptr;
    lv_obj_t* master_switch_ = nullptr;
    lv_obj_t* local_panel_ = nullptr;
    lv_obj_t* settings_panel_ = nullptr;
    lv_obj_t* speaker_panel_ = nullptr;
    lv_obj_t* current_count_ = nullptr;
    lv_obj_t* current_list_ = nullptr;
    lv_obj_t* profile_buttons_[2] = {};
    lv_obj_t* nearby_count_ = nullptr;
    lv_obj_t* device_list_ = nullptr;
    lv_obj_t* nearby_spinner_ = nullptr;
    lv_obj_t* scan_button_ = nullptr;
    lv_obj_t* scan_label_ = nullptr;
    std::vector<lv_obj_t*> device_rows_;
    std::vector<lv_obj_t*> device_actions_;
    std::size_t nearby_device_count_ = 0;
};

}  // namespace agent_ui::bluetooth_settings_ui
