#pragma once

#include <string>

#include "lvgl.h"

namespace agent_ui::network_dialogs_ui {

class View {
public:
    void Attach(lv_obj_t* screen);
    void Reset();

    bool HasPassword() const { return password_overlay_ != nullptr; }
    bool HasStatus() const { return status_overlay_ != nullptr; }
    const char* Password() const;

    void OpenPassword(const char* ssid, lv_event_cb_t connect_callback,
                      void* user_data = nullptr);
    void ClosePassword();
    void CloseStatus();
    void OpenConnecting(const char* ssid);
    void OpenFailure(const char* title, const char* detail);
    void OpenRestart(const char* headline, int seconds);
    void UpdateRestart(int seconds);
    void OpenNetworkSwitch(const char* target_name);
    void OpenSimSwitch(const char* slot_name);
    void SetStatusMessage(const char* text);

private:
    static void OnShowPassword(lv_event_t* event);
    static void OnCancelPassword(lv_event_t* event);
    lv_obj_t* CreateOverlay();
    lv_obj_t* CreateStatusCard();

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* password_overlay_ = nullptr;
    lv_obj_t* password_textarea_ = nullptr;
    lv_obj_t* status_overlay_ = nullptr;
    lv_obj_t* status_message_ = nullptr;
};

}  // namespace agent_ui::network_dialogs_ui
