#include "lv_adapter_display.h"

#include <cstring>
#include <cinttypes>
#include <memory>

#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <soc/soc_caps.h>

#if SOC_MIPI_DSI_SUPPORTED
#include <esp_lcd_mipi_dsi.h>
#endif

#include "esp_lv_adapter.h"
#include "expression_acceleration.h"
#include "touch_feed.h"   

#include "agent_ui/agent_ui_runtime.h"
#include "agent_ui/apps/boot/boot_view.h"
#include "device_state.h"

#include "application.h"

static const char* TAG = "LVAdapterDisplay";

LVAdapterDisplay::LVAdapterDisplay(const esp_lcd_panel_handle_t panel,
                                   const esp_lcd_panel_io_handle_t panel_io,
                                   const esp_lcd_touch_handle_t touch_handle, const int width,
                                   const int height)
    : panel_(panel) {
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.stack_in_psram = true;
    // Keep rendering ahead of low-rate sensor/I2C maintenance tasks. Audio and
    // networking still use higher priorities, so UI work cannot starve them.
    adapter_cfg.task_priority = 4;
    adapter_cfg.task_core_id = 1;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    // 性能调优要点（720x720 RGB565 屏）：
    //   - enable_ppa_accel: 开启 PPA。半透明/圆角会触发 adapter「先 msync 再
    //     软件 fallback」；主屏翻页期间由 home_screen 降级为纯不透明直角绘制
    //     （见 SetPagerSkeletonMode），避免刷 invalid addr。
    //   - tear_avoid_mode = TRIPLE_FULL：直接把 LCD 驱动里 num_fbs=3 的 3 张
    //     panel 帧缓冲（PSRAM 上 3×720×720×2 ≈ 3MB）当成 LVGL 的 draw buffer
    //     用，渲染→DMA 三级流水，无撕裂。
    //     之前用 DEFAULT_MIPI_DSI（= TRIPLE_PARTIAL）会额外要一块
    //     720×buffer_height×2 ≈ 280KB 的内部 SRAM partial buffer，而片上 SRAM
    //     被 FreeRTOS / WiFi / SDIO 吃掉后根本剩不下，导致启动日志里报
    //     「alloc partial draw buffer failed」+「tear mode 4 setup failed」，
    //     adapter 还会再 fallback 申请 ~576KB PSRAM 当双缓冲，3MB+576KB 双重
    //     浪费。TRIPLE_FULL 彻底避开这条 fallback 路径。
    //   - buffer_height / require_double_buffer 在 TRIPLE_FULL 模式下不再生效
    //     （buffer 直接用 panel FB），保留是为了将来切回 partial 模式方便。
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = panel_io,
        .profile =
            {
                .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
                .hor_res = static_cast<uint16_t>(width),
                .ver_res = static_cast<uint16_t>(height),
                .buffer_height = 200,
                .use_psram = true,
                .enable_ppa_accel = true,
                .require_double_buffer = true,
            },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL,
    };

    display_ = esp_lv_adapter_register_display(&disp_cfg);
    ESP_ERROR_CHECK(esp_lv_adapter_fps_stats_enable(display_, true));
    agent_ui::InitializeExpressionAcceleration();
    esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display_, touch_handle);
    lv_indev_t* touch_indev = esp_lv_adapter_register_touch(&touch_cfg);
    touch_feed_init(touch_handle, 20);
    touch_feed_attach_indev(touch_indev);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetupUI();
        esp_lv_adapter_unlock();
    }

    // Application::GetInstance().ForceReturnToIdle();
}

void LVAdapterDisplay::SetupUI() {
    agent_ui::Runtime::Get().Initialize();
    lv_obj_t* boot_scr = agent_ui::BootView::Create();
    lv_screen_load(boot_scr);

    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                agent_ui::Runtime::Get().Start();
                esp_lv_adapter_unlock();
            }

            lv_timer_delete(t);
        },
        2000, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}

LVAdapterDisplay::~LVAdapterDisplay() = default;

void LVAdapterDisplay::SetEmotion(const char* const emotion) {
    ESP_LOGD(TAG, "AI emotion update: %s", emotion != nullptr ? emotion : "<null>");
    if (emotion == nullptr || std::strcmp(emotion, "dizzy") != 0) return;
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;
    agent_ui::Runtime::Get().PlayDizzyExpression();
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetChatMessage(const char* const role, const char* const content) {
    if (role == nullptr || content == nullptr || content[0] == '\0') {
        return;
    }

    // Only real conversation content belongs in the home AI area. Startup
    // and protocol messages use the system role and must not replace the
    // idle greeting with a firmware version or connection status.
    const bool is_user = (std::strcmp(role, "user") == 0);
    const bool is_assistant = (std::strcmp(role, "assistant") == 0);
    if (!is_user && !is_assistant) {
        return;
    }

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    agent_ui::Runtime::Get().SetConversationMessage(role, content);
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetStatus(const char* const status) {
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;
    auto& ui = agent_ui::Runtime::Get();
    ui.SetSystemStatus(status);
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateConnecting:
            ui.SetAgentState(agent_ui::AgentState::Connecting);
            break;
        case kDeviceStateListening:
            ui.SetAgentState(agent_ui::AgentState::Listening);
            break;
        case kDeviceStateSpeaking:
            ui.SetAgentState(agent_ui::AgentState::Answering);
            break;
        default:
            ui.SetAgentState(agent_ui::AgentState::Idle);
            break;
    }
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::ShowNotification(const char* notification, int duration_ms) {}

void LVAdapterDisplay::UpdateStatusBar(bool update_all) {}

void LVAdapterDisplay::SetPowerSaveMode(bool on) {
    (void)SetPowerSaveModeChecked(on);
}

bool LVAdapterDisplay::SetPowerSaveModeChecked(bool on) {
    if (panel_ == nullptr) return false;
    if (power_save_mode_ == on) return true;

    const esp_err_t err = esp_lcd_panel_disp_on_off(panel_, !on);
    if (err == ESP_OK) {
        power_save_mode_ = on;
        ESP_LOGI(TAG, "panel power save %s", on ? "on" : "off");
        return true;
    } else {
        ESP_LOGW(TAG, "panel power save %s failed: %s",
                 on ? "on" : "off", esp_err_to_name(err));
        return false;
    }
}

bool LVAdapterDisplay::SetDiagnosticPattern(DisplayDiagnosticPattern pattern) {
#if SOC_MIPI_DSI_SUPPORTED
    mipi_dsi_pattern_type_t dsi_pattern = MIPI_DSI_PATTERN_NONE;
    switch (pattern) {
        case DisplayDiagnosticPattern::None:
            break;
        case DisplayDiagnosticPattern::ColorBarsVertical:
            dsi_pattern = MIPI_DSI_PATTERN_BAR_VERTICAL;
            break;
        case DisplayDiagnosticPattern::ColorBarsHorizontal:
            dsi_pattern = MIPI_DSI_PATTERN_BAR_HORIZONTAL;
            break;
    }
    const esp_err_t error = esp_lcd_dpi_panel_set_pattern(panel_, dsi_pattern);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set DSI diagnostic pattern %d: %s",
                 static_cast<int>(pattern), esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(TAG, "DSI diagnostic pattern=%d", static_cast<int>(pattern));
    return true;
#else
    (void)pattern;
    ESP_LOGW(TAG, "DSI diagnostic patterns are not supported on this target");
    return false;
#endif
}

void LVAdapterDisplay::SetPreviewImage(const void* image) {}

void LVAdapterDisplay::SetTheme(Theme* const theme) { ESP_LOGI(TAG, "SetTheme: %p", theme); }

bool LVAdapterDisplay::Lock(const int timeout_ms) {
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

void LVAdapterDisplay::Unlock() { esp_lv_adapter_unlock(); }
