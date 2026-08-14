#include "power_key.h"

#include <esp_log.h>

#include "IOExpander.hpp"
#include "lvgl.h"
#include "apps/power/power_view.h"
#include "apps/standby/standby_view.h"

namespace agent_ui {
namespace {
constexpr char kTag[] = "AgentPowerKey";
constexpr uint32_t kLongPressMs = 1500;
bool s_initialized = false;

void OnShortAsync(void*) {
    ESP_LOGI(kTag, "Short press dispatched to LVGL (standby=%d screen_off=%d)",
             StandbyView::IsActive(), StandbyView::IsScreenOff());
    StandbyView::HandlePowerKey();
}

void OnLongAsync(void*) {
    ESP_LOGI(kTag, "Long press dispatched to LVGL (standby=%d screen_off=%d)",
             StandbyView::IsActive(), StandbyView::IsScreenOff());
    if (StandbyView::IsActive()) {
        StandbyView::HandlePowerKey();
    } else {
        PowerView::ShowDialog();
    }
}

void OnShort() {
    ESP_LOGI(kTag, "Short press detected by IO expander");
    lv_async_call(OnShortAsync, nullptr);
}

void OnLong() {
    ESP_LOGI(kTag, "Long press detected by IO expander");
    lv_async_call(OnLongAsync, nullptr);
}
}

void PowerKey::Initialize() {
    if (s_initialized) return;
    auto& io = IOExpander::getInstance();
    const esp_err_t result = io.onShortOrLongPress(
        IOExpander::Pin::PWR_KEY, kLongPressMs, OnShort, OnLong);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Power key registration failed: %d", result);
        return;
    }
    s_initialized = true;
}

}  // namespace agent_ui
