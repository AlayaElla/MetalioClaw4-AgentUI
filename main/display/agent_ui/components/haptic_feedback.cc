#include "haptic_feedback.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace agent_ui {
namespace {

constexpr char kTag[] = "HapticFeedback";
constexpr gpio_num_t kMotorPin = GPIO_NUM_22;
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_1;
constexpr uint32_t kPwmFrequencyHz = 4000;
constexpr uint32_t kMaxDuty = (1U << 10) - 1;

esp_timer_handle_t s_stop_timer = nullptr;
bool s_initialized = false;
int64_t s_last_light_pulse_us = 0;

constexpr int64_t kLightPulseIntervalUs = 35 * 1000;

void StopMotor(void*) {
    if (!s_initialized) return;
    ledc_set_duty(kSpeedMode, kChannel, 0);
    ledc_update_duty(kSpeedMode, kChannel);
}

bool EnsureInitialized() {
    if (s_initialized) return true;

    const ledc_timer_config_t timer = {
        .speed_mode = kSpeedMode,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = kTimer,
        .freq_hz = kPwmFrequencyHz,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    if (ledc_timer_config(&timer) != ESP_OK) return false;

    const ledc_channel_config_t channel = {
        .gpio_num = kMotorPin,
        .speed_mode = kSpeedMode,
        .channel = kChannel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = kTimer,
        .duty = 0,
        .hpoint = 0,
        .flags = {.output_invert = 0},
    };
    if (ledc_channel_config(&channel) != ESP_OK) return false;

    const esp_timer_create_args_t timer_args = {
        .callback = StopMotor,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "haptic_stop",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &s_stop_timer) != ESP_OK) return false;

    s_initialized = true;
    ESP_LOGI(kTag, "vibration motor ready on GPIO %d", kMotorPin);
    return true;
}

void OnButtonClicked(lv_event_t*) {
    PlayHaptic(HapticStrength::Medium);
}

}  // namespace

void PlayHaptic(HapticStrength strength) {
    if (strength == HapticStrength::Light) {
        const int64_t now_us = esp_timer_get_time();
        if (s_last_light_pulse_us != 0 &&
            now_us - s_last_light_pulse_us < kLightPulseIntervalUs) {
            return;
        }
        s_last_light_pulse_us = now_us;
    }

    if (!EnsureInitialized()) {
        ESP_LOGW(kTag, "failed to initialize vibration motor");
        return;
    }

    uint32_t duty;
    uint64_t duration_us;
    if (strength == HapticStrength::Light) {
        // A short, strong kick reliably overcomes the ERM motor's startup
        // threshold while still feeling lighter than the longer click pulse.
        duty = kMaxDuty * 70 / 100;
        duration_us = 25 * 1000;
    } else {
        duty = kMaxDuty * 65 / 100;
        duration_us = 45 * 1000;
    }

    esp_timer_stop(s_stop_timer);
    ledc_set_duty(kSpeedMode, kChannel, duty);
    ledc_update_duty(kSpeedMode, kChannel);
    esp_timer_start_once(s_stop_timer, duration_us);
}

void AttachButtonHaptic(lv_obj_t* button) {
    if (button == nullptr) return;
    lv_obj_add_event_cb(button, OnButtonClicked, LV_EVENT_CLICKED, nullptr);
}

}  // namespace agent_ui
