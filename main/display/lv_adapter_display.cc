#include "lv_adapter_display.h"

#include <cstring>
#include <cinttypes>
#include <memory>
#include <utility>

#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <soc/soc_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if SOC_MIPI_DSI_SUPPORTED
#include <esp_lcd_mipi_dsi.h>
#endif

#include "esp_lv_adapter.h"
#include "expression_acceleration.h"
#include "touch_feed.h"   
#include "ui_dispatcher.h"

#include "agent_ui/agent_ui_runtime.h"
#include "agent_ui/apps/boot/boot_view.h"
#include "agent_ui/apps/codex/codex_media_cache.h"
#include "agent_ui/components/render_snapshot_buffer.h"
#include "agent_ui/core/status_bar.h"
#include "device_state.h"

#include "application.h"
#include "mipi_dsi_power_control.h"
#include "panel_transport_lifecycle.h"
#include "panel_sleep_transaction.h"

namespace {

const char* TAG = "LVAdapterDisplay";

#if AGENT_UI_EXPERIMENTAL_DIRECT_RENDER
constexpr auto kDisplayTearAvoidMode =
    ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
constexpr char kDisplayRenderModeName[] = "DOUBLE_DIRECT";
#else
constexpr auto kDisplayTearAvoidMode =
    ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL;
constexpr char kDisplayRenderModeName[] = "TRIPLE_FULL";
#endif

int64_t ReadDisplayTelemetryClock(void*) { return esp_timer_get_time(); }

void RequestUiDispatcherWake() {
    const esp_err_t err = esp_lv_adapter_notify_work();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "UI dispatcher work notification failed: %s",
                 esp_err_to_name(err));
    }
}

void DrainUiDispatcherBeforeTimers(void*) {
    (void)UiDispatcher::DrainPending();
}

void LogDisplayTelemetryReport(const display_telemetry::WindowReport& stats,
                               void*) {
    const auto cache = agent_ui::RenderSnapshotBuffer::GetCacheStats();
    const auto media = agent_ui::codex_media::Cache::GetImageMemoryStats();
    constexpr uint32_t kInternal8Bit = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t kPsram8Bit = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t internal_free = heap_caps_get_free_size(kInternal8Bit);
    const size_t internal_min = heap_caps_get_minimum_free_size(kInternal8Bit);
    const size_t internal_largest = heap_caps_get_largest_free_block(kInternal8Bit);
    const size_t psram_free = heap_caps_get_free_size(kPsram8Bit);
    const size_t psram_min = heap_caps_get_minimum_free_size(kPsram8Bit);
    const size_t psram_largest = heap_caps_get_largest_free_block(kPsram8Bit);
    const size_t owner_stack_hwm_bytes =
        static_cast<size_t>(uxTaskGetStackHighWaterMark(nullptr)) * sizeof(StackType_t);
    ESP_LOGI(TAG, "render mode=%s frames=%" PRIu64 " cycles_s=%.2f"
                  " avg_refresh_ms=%.2f max_refresh_ms=%.2f dirty_px=%" PRIu64
                  " flush_cb_ms_total=%.2f flush_wait_ms_total=%.2f window_ms=%" PRIu64
                  " snapshot_cache=%u/%u B rejected=%u alloc_fail=%u reuse=%u"
                  " media_images=%u/%u B rejected=%u"
                  " heap8_internal_free/min/largest=%u/%u/%u B"
                  " heap8_psram_free/min/largest=%u/%u/%u B"
                  " lvgl_owner_stack_hwm=%u B",
             stats.render_mode, stats.rendered_frames,
             static_cast<double>(stats.rendered_frames) * 1000000.0 / stats.window_us,
             stats.refresh_total_us / (1000.0 * stats.rendered_frames),
             stats.refresh_max_us / 1000.0, stats.dirty_pixels,
             stats.flush_callback_total_us / 1000.0,
             stats.flush_wait_total_us / 1000.0, stats.window_us / 1000,
             static_cast<unsigned>(cache.current_charge_bytes),
             static_cast<unsigned>(cache.peak_charge_bytes),
             static_cast<unsigned>(cache.rejected_allocations),
             static_cast<unsigned>(cache.allocation_failures),
             static_cast<unsigned>(cache.reuse_hits),
             static_cast<unsigned>(media.current_bytes),
             static_cast<unsigned>(media.peak_bytes),
             static_cast<unsigned>(media.rejected_allocations),
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_min),
             static_cast<unsigned>(internal_largest),
             static_cast<unsigned>(psram_free),
             static_cast<unsigned>(psram_min),
             static_cast<unsigned>(psram_largest),
             static_cast<unsigned>(owner_stack_hwm_bytes));
}

}  // namespace

LVAdapterDisplay::LVAdapterDisplay(const esp_lcd_panel_handle_t panel,
                                   const esp_lcd_panel_io_handle_t panel_io,
                                   const esp_lcd_touch_handle_t touch_handle, const int width,
                                   const int height,
                                   std::function<esp_err_t(esp_lcd_panel_handle_t*,
                                                           esp_lcd_panel_io_handle_t*)> create_panel,
                                   std::function<void(esp_lcd_panel_handle_t, bool)> update_panel_state,
                                   std::function<esp_err_t(esp_lcd_panel_handle_t)> ensure_panel_dma2d,
                                   std::function<esp_err_t(esp_lcd_panel_handle_t)> delete_panel,
                                   std::function<esp_err_t()> delete_panel_transport,
                                   std::function<esp_err_t(esp_lcd_panel_handle_t, bool)> set_panel_power,
                                   std::function<esp_err_t(esp_lcd_panel_io_handle_t)> publish_panel_io)
    : panel_(panel), panel_io_(panel_io), create_panel_(std::move(create_panel)),
      update_panel_state_(std::move(update_panel_state)),
      ensure_panel_dma2d_(std::move(ensure_panel_dma2d)),
      delete_panel_(std::move(delete_panel)),
      delete_panel_transport_(std::move(delete_panel_transport)),
      set_panel_power_(std::move(set_panel_power)),
      publish_panel_io_(std::move(publish_panel_io)),
      display_telemetry_(kDisplayRenderModeName) {
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.stack_in_psram = true;
    // Keep rendering ahead of low-rate sensor/I2C maintenance tasks. Audio and
    // networking still use higher priorities, so UI work cannot starve them.
    adapter_cfg.task_priority = 4;
    adapter_cfg.task_core_id = 1;

    if (!UiDispatcher::ConfigureExecutor(RequestUiDispatcherWake)) {
        ESP_LOGE(TAG, "UI dispatcher owner-task executor could not be configured");
    }
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));
    ESP_ERROR_CHECK(esp_lv_adapter_set_before_handler_callback(
        DrainUiDispatcherBeforeTimers, nullptr));

    // 720x720 RGB888 uses 3 bytes per pixel. The default TRIPLE_FULL mode
    // uses the panel's three framebuffers (~4.45 MiB) as LVGL draw buffers;
    // DOUBLE_DIRECT uses the adapter's two-buffer direct-render mode.
    // buffer_height and require_double_buffer do not size these full buffers.
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
        .tear_avoid_mode = kDisplayTearAvoidMode,
    };

    display_ = esp_lv_adapter_register_display(&disp_cfg);
    display_telemetry_.Attach(display_, ReadDisplayTelemetryClock,
                              LogDisplayTelemetryReport, nullptr);
    ESP_LOGI(TAG, "Rendering mode=%s",
             kDisplayRenderModeName);
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

void LVAdapterDisplay::UpdateStatusBar(bool update_all) {
    // The status bar already has a periodic timer. A connection event requests
    // an earlier UI refresh; never block the network event loop on the UI lock.
    if (!update_all || esp_lv_adapter_lock(0) != ESP_OK) return;
    agent_ui::StatusBar::Get().RefreshAsync();
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetPowerSaveMode(bool on) {
    (void)SetPowerSaveModeChecked(on);
}

bool LVAdapterDisplay::SetPowerSaveModeChecked(bool on) {
    if (!on) return PrepareWakeFromPowerSave();
    std::lock_guard<std::mutex> lock(power_save_mutex_);
    if (on) {
        if (power_save_mode_.load()) return true;
        if (panel_ == nullptr) return false;
        struct SleepOps {
            LVAdapterDisplay& owner;
            bool sleep_panel() {
                const esp_err_t err = owner.set_panel_power_
                                          ? owner.set_panel_power_(owner.panel_, false)
                                          : esp_lcd_panel_disp_on_off(owner.panel_, false);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "panel sleep command failed: %s", esp_err_to_name(err));
                    return false;
                }
                return true;
            }
            bool prepare_adapter() {
                const esp_err_t err = esp_lv_adapter_sleep_prepare();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "LVGL display detach failed: %s", esp_err_to_name(err));
                    return false;
                }
                owner.adapter_sleep_prepared_ = true;
                return true;
            }
            bool disable_dma2d() {
                const esp_err_t err = esp_lcd_dpi_panel_disable_dma2d(owner.panel_);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "disable panel DMA2D failed: %s", esp_err_to_name(err));
                    return false;
                }
                if (owner.update_panel_state_) owner.update_panel_state_(owner.panel_, false);
                return true;
            }
            bool delete_panel() {
                const esp_err_t err = owner.delete_panel_
                                          ? owner.delete_panel_(owner.panel_)
                                          : esp_lcd_panel_del(owner.panel_);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "delete DPI panel failed: %s", esp_err_to_name(err));
                    return false;
                }
                owner.panel_ = nullptr;
                if (owner.update_panel_state_) owner.update_panel_state_(nullptr, false);
                metalio_mipi_dsi_power_panel_deleted();
                return true;
            }
            bool delete_transport() {
                if (!owner.delete_panel_transport_) return true;
                const esp_err_t err = owner.delete_panel_transport_();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "delete MIPI DSI transport failed: %s",
                             esp_err_to_name(err));
                    return false;
                }
                owner.panel_io_ = nullptr;
                owner.panel_present_.store(false);
                owner.panel_io_published_ = false;
                return true;
            }
            bool restore_dma2d() {
                const esp_err_t err = esp_lcd_dpi_panel_enable_dma2d(owner.panel_);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "restore panel DMA2D failed: %s", esp_err_to_name(err));
                    return false;
                }
                if (owner.update_panel_state_) owner.update_panel_state_(owner.panel_, true);
                return true;
            }
            bool recover_adapter() {
                if (!owner.adapter_sleep_prepared_) return true;
                const esp_err_t err = esp_lv_adapter_sleep_recover(
                    owner.display_, owner.panel_, owner.panel_io_);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "adapter rollback failed: %s", esp_err_to_name(err));
                    return false;
                }
                owner.adapter_sleep_prepared_ = false;
                return true;
            }
            bool wake_panel() {
                const esp_err_t err = owner.set_panel_power_
                                          ? owner.set_panel_power_(owner.panel_, true)
                                          : esp_lcd_panel_disp_on_off(owner.panel_, true);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "panel rollback wake failed: %s", esp_err_to_name(err));
                    return false;
                }
                return true;
            }
            void mark_recovery_required() {
                owner.power_save_mode_.store(true);
                if (owner.panel_ == nullptr) owner.panel_present_.store(true);
            }
        } ops{*this};

        const auto result = display_power::RunPanelSleepTransaction(ops);
        if (result != display_power::PanelSleepResult::Slept) {
            ESP_LOGW(TAG, "display sleep transaction failed at stage=%u",
                     static_cast<unsigned>(result));
            return false;
        }
        power_save_mode_.store(true);
        ESP_LOGI(TAG, "DPI producer stopped; panel deleted and driver lock released");
        return true;
    }

    return false;
}

bool LVAdapterDisplay::PrepareWakeFromPowerSave() {
    // Match the UI worker's lock order (LVGL adapter, then display state).
    // sleep_recover() takes the adapter's recursive lock internally and wakes
    // its worker, which remains blocked here until panel_ and state are ready.
    const esp_err_t adapter_lock = esp_lv_adapter_lock(-1);
    if (adapter_lock != ESP_OK) {
        ESP_LOGE(TAG, "lock LVGL adapter before panel recovery failed: %s",
                 esp_err_to_name(adapter_lock));
        return false;
    }
    struct AdapterUnlock {
        ~AdapterUnlock() { esp_lv_adapter_unlock(); }
    } adapter_unlock;
    std::lock_guard<std::mutex> lock(power_save_mutex_);
    if (!power_save_mode_.load()) {
        return panel_ != nullptr;
    }
    if (panel_ == nullptr) {
        if (!create_panel_) {
            ESP_LOGE(TAG, "panel factory is unavailable during wake");
            return false;
        }
        // Ensure the producer can start immediately; its IDF-owned lock then
        // remains the final bandwidth guard while the new panel is alive.
        esp_err_t err = metalio_mipi_dsi_power_set_frequency(360);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "restore 360 MHz before panel start failed: %s",
                     esp_err_to_name(err));
            return false;
        }
        esp_lcd_panel_handle_t new_panel = nullptr;
        esp_lcd_panel_io_handle_t new_panel_io = nullptr;
        err = create_panel_(&new_panel, &new_panel_io);
        if (err != ESP_OK || new_panel == nullptr || new_panel_io == nullptr) {
            // A factory can return an owned but only partially initialized
            // panel when cleanup itself fails. Keep the physical-presence
            // guard conservative; the board factory owns its retry cleanup.
            if (new_panel != nullptr) panel_present_.store(true);
            ESP_LOGE(TAG, "recreate DPI panel failed: %s",
                     esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
            return false;
        }
        panel_ = new_panel;
        panel_io_ = new_panel_io;
        panel_io_published_ = false;
        panel_present_.store(true);
        if (update_panel_state_) update_panel_state_(new_panel, true);
    }

    struct WakeOps {
        LVAdapterDisplay& owner;
        esp_lcd_panel_io_handle_t panel_io() const { return owner.panel_io_; }
        bool ensure_dma2d() {
            if (!owner.ensure_panel_dma2d_) return true;
            const esp_err_t err = owner.ensure_panel_dma2d_(owner.panel_);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "enable DMA2D before recovery failed: %s",
                         esp_err_to_name(err));
                return false;
            }
            return true;
        }
        bool wake_panel() {
            const esp_err_t err = owner.set_panel_power_
                                      ? owner.set_panel_power_(owner.panel_, true)
                                      : esp_lcd_panel_disp_on_off(owner.panel_, true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "enable panel before recovery failed: %s",
                         esp_err_to_name(err));
                return false;
            }
            return true;
        }
        bool recover_adapter(esp_lcd_panel_io_handle_t fresh_io) {
            if (!owner.adapter_sleep_prepared_) return true;
            if (fresh_io == nullptr || fresh_io != owner.panel_io_) return false;
            const esp_err_t err =
                esp_lv_adapter_sleep_recover(owner.display_, owner.panel_, fresh_io);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "recover LVGL display failed: %s", esp_err_to_name(err));
                return false;
            }
            owner.adapter_sleep_prepared_ = false;
            return true;
        }
        bool publish_io() {
            if (owner.panel_io_published_) return true;
            if (!owner.publish_panel_io_) return false;
            const esp_err_t err = owner.publish_panel_io_(owner.panel_io_);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "publish ready panel IO failed: %s", esp_err_to_name(err));
                return false;
            }
            owner.panel_io_published_ = true;
            return true;
        }
    } ops{*this};

    const auto result = display_power::RunPanelWakeTransaction(ops);
    if (result != display_power::PanelWakeResult::Recovered) {
        ESP_LOGE(TAG, "panel wake transaction failed at stage=%u",
                 static_cast<unsigned>(result));
        power_save_mode_.store(true);
        return false;
    }
    panel_present_.store(true);
    power_save_mode_.store(false);
    ESP_LOGI(TAG, "MIPI DSI transport reset; fresh panel IO rebound and LVGL resumed");
    return true;
}

bool LVAdapterDisplay::IsPowerSaveActive() const {
    return power_save_mode_.load();
}

bool LVAdapterDisplay::IsPanelPresent() const { return panel_present_.load(); }

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
