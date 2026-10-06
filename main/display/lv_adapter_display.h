#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <cstdint>
#include "display.h"
#include "display_render_telemetry.h"
#include "esp_lv_adapter.h"
#include "lvgl_font.h"

class LVAdapterDisplay : public Display {
public:
    LVAdapterDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                     const esp_lcd_touch_handle_t touch_handle, int width, int height,
                     std::function<esp_err_t(esp_lcd_panel_handle_t*,
                                             esp_lcd_panel_io_handle_t*)> create_panel,
                     std::function<void(esp_lcd_panel_handle_t, bool)> update_panel_state,
                     std::function<esp_err_t(esp_lcd_panel_handle_t)> ensure_panel_dma2d,
                     std::function<esp_err_t(esp_lcd_panel_handle_t)> delete_panel,
                     std::function<esp_err_t()> delete_panel_transport,
                     std::function<esp_err_t(esp_lcd_panel_handle_t, bool)> set_panel_power,
                     std::function<esp_err_t(esp_lcd_panel_io_handle_t)> publish_panel_io);
    virtual ~LVAdapterDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual bool SetPowerSaveModeChecked(bool on) override;
    virtual bool PrepareWakeFromPowerSave() override;
    virtual bool IsPowerSaveActive() const override;
    virtual bool IsPanelPresent() const override;
    virtual bool SetDiagnosticPattern(DisplayDiagnosticPattern pattern) override;
    virtual void SetPreviewImage(const void* image);

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    void SetupUI();

    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    lv_display_t* display_ = nullptr;
    std::function<esp_err_t(esp_lcd_panel_handle_t*,
                             esp_lcd_panel_io_handle_t*)> create_panel_;
    std::function<void(esp_lcd_panel_handle_t, bool)> update_panel_state_;
    std::function<esp_err_t(esp_lcd_panel_handle_t)> ensure_panel_dma2d_;
    std::function<esp_err_t(esp_lcd_panel_handle_t)> delete_panel_;
    std::function<esp_err_t()> delete_panel_transport_;
    std::function<esp_err_t(esp_lcd_panel_handle_t, bool)> set_panel_power_;
    std::function<esp_err_t(esp_lcd_panel_io_handle_t)> publish_panel_io_;
    mutable std::mutex power_save_mutex_;
    std::atomic<bool> power_save_mode_{false};
    std::atomic<bool> panel_present_{true};
    std::atomic<bool> adapter_sleep_prepared_{false};
    bool panel_io_published_ = true;
    display_telemetry::DisplayRenderTelemetry display_telemetry_;
};
