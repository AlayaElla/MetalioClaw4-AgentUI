#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include "display.h"
#include "esp_lv_adapter.h"
#include "lvgl_font.h"

class LVAdapterDisplay : public Display {
public:
    LVAdapterDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                     const esp_lcd_touch_handle_t touch_handle, int width, int height);
    virtual ~LVAdapterDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual bool SetPowerSaveModeChecked(bool on) override;
    virtual bool SetDiagnosticPattern(DisplayDiagnosticPattern pattern) override;
    virtual void SetPreviewImage(const void* image);

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    void SetupUI();

    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_display_t* display_ = nullptr;
    bool power_save_mode_ = false;
};
