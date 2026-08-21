#include "sc7a20_motion.h"

#include <algorithm>
#include <array>
#include <cstdlib>

#include "application.h"
#include "display/agent_ui/agent_ui_runtime.h"
#include "display/agent_ui/apps/standby/standby_view.h"
#include "display/agent_ui/core/idle_power.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "SC7A20";

constexpr std::array<uint8_t, 2> kAddresses = {0x18, 0x19};
constexpr uint8_t kWhoAmIRegister = 0x0F;
constexpr uint8_t kWhoAmIValue = 0x11;
constexpr uint8_t kCtrlReg1 = 0x20;
constexpr uint8_t kCtrlReg4 = 0x23;
constexpr uint8_t kOutputStart = 0x28;
constexpr uint8_t kAutoIncrement = 0x80;
constexpr uint8_t kPowerDown = 0x00;
constexpr uint8_t kOdr50HzAllAxes = 0x47;
constexpr uint8_t kBduHighResolution2g = 0x88;

constexpr uint32_t kI2cSpeedHz = 100 * 1000;
constexpr int kI2cTimeoutMs = 50;
constexpr int kProbeTimeoutMs = 50;
constexpr uint32_t kSamplePeriodMs = 20;
constexpr uint32_t kReconnectDelayMs = 10 * 1000;
constexpr int kMaxConsecutiveReadErrors = 5;

// Shake tuning at 50 Hz. Motion must stay energetic for about half a second
// and reverse direction at least three times. A burst is finalized after a
// short quiet gap so the AI message can include the whole burst's count.
constexpr int32_t kStrongMotionMg = 550;
constexpr int64_t kReversalDotThreshold = -100000;
constexpr uint32_t kMinReversalIntervalMs = 60;
constexpr uint32_t kSustainMs = 450;
constexpr uint32_t kMaxStrongGapMs = 180;
constexpr int kMinStrongSamples = 12;
constexpr int kMinReversals = 3;
constexpr uint32_t kTriggerCooldownMs = 3000;
constexpr uint32_t kShakeCountResetMs = 5U * 60U * 1000U;
constexpr int kGravityFilterShift = 5;

// Tilt uses the low-frequency gravity component already present in each
// accelerometer sample. A 30 mg dead zone keeps a level device still, while
// 700 mg maps to full parallax travel (roughly a 45-degree lean).
constexpr int kTiltDeadZoneMg = 30;
constexpr int kTiltFullScaleMg = 700;
constexpr int kTiltFilterDivisor = 8;

uint32_t NowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

int16_t DecodeAxis(uint8_t low, uint8_t high) {
    const uint16_t combined = static_cast<uint16_t>(low) |
                              (static_cast<uint16_t>(high) << 8);
    const int16_t raw = static_cast<int16_t>(combined);
    // In ±2 g high-resolution mode the 12-bit sample is left-aligned and its
    // sensitivity is 1 mg/LSB after removing the four fractional bits.
    return static_cast<int16_t>(raw / 16);
}

int32_t NormalizeTiltQ10(int16_t millig) {
    const int32_t magnitude = std::abs(static_cast<int32_t>(millig));
    if (magnitude <= kTiltDeadZoneMg) return 0;
    const int32_t usable = magnitude - kTiltDeadZoneMg;
    const int32_t range = kTiltFullScaleMg - kTiltDeadZoneMg;
    const int32_t normalized = std::min<int32_t>(1000, usable * 1000 / range);
    return millig < 0 ? -normalized : normalized;
}

}  // namespace

int Sc7a20ShakeDetector::Update(const Sc7a20Sample& sample, uint32_t now_ms) {
    const int32_t values[3] = {sample.x_mg, sample.y_mg, sample.z_mg};
    if (!gravity_initialized_) {
        for (int i = 0; i < 3; ++i) gravity_q4_[i] = values[i] * 16;
        gravity_initialized_ = true;
        return 0;
    }

    Vector dynamic;
    int32_t* components[3] = {&dynamic.x, &dynamic.y, &dynamic.z};
    for (int i = 0; i < 3; ++i) {
        const int32_t sample_q4 = values[i] * 16;
        gravity_q4_[i] += (sample_q4 - gravity_q4_[i]) >> kGravityFilterShift;
        *components[i] = values[i] - gravity_q4_[i] / 16;
    }

    if (has_triggered_ && now_ms - last_trigger_ms_ < kTriggerCooldownMs) {
        ResetCandidate();
        return 0;
    }

    const int32_t motion_l1 = std::abs(dynamic.x) + std::abs(dynamic.y) +
                              std::abs(dynamic.z);
    if (motion_l1 < kStrongMotionMg) {
        if (candidate_active_ && now_ms - last_strong_ms_ > kMaxStrongGapMs) {
            return FinalizeCandidate(now_ms);
        }
        return 0;
    }

    if (!candidate_active_) {
        StartCandidate(dynamic, now_ms);
        return 0;
    }

    last_strong_ms_ = now_ms;
    ++strong_samples_;

    const int64_t dot = static_cast<int64_t>(dynamic.x) * lobe_.x +
                        static_cast<int64_t>(dynamic.y) * lobe_.y +
                        static_cast<int64_t>(dynamic.z) * lobe_.z;
    if (dot <= kReversalDotThreshold &&
        now_ms - last_reversal_ms_ >= kMinReversalIntervalMs) {
        ++reversal_count_;
        last_reversal_ms_ = now_ms;
        lobe_ = dynamic;
    } else if (dot > 0) {
        lobe_.x = (lobe_.x * 3 + dynamic.x) / 4;
        lobe_.y = (lobe_.y * 3 + dynamic.y) / 4;
        lobe_.z = (lobe_.z * 3 + dynamic.z) / 4;
    }

    return 0;
}

void Sc7a20ShakeDetector::Reset() {
    gravity_initialized_ = false;
    has_triggered_ = false;
    last_trigger_ms_ = 0;
    for (auto& gravity : gravity_q4_) gravity = 0;
    ResetCandidate();
}

void Sc7a20ShakeDetector::ResetCandidate() {
    candidate_active_ = false;
    candidate_start_ms_ = 0;
    last_strong_ms_ = 0;
    last_reversal_ms_ = 0;
    strong_samples_ = 0;
    reversal_count_ = 0;
    lobe_ = {};
}

void Sc7a20ShakeDetector::StartCandidate(const Vector& dynamic,
                                         uint32_t now_ms) {
    candidate_active_ = true;
    candidate_start_ms_ = now_ms;
    last_strong_ms_ = now_ms;
    last_reversal_ms_ = now_ms;
    strong_samples_ = 1;
    reversal_count_ = 0;
    lobe_ = dynamic;
}

int Sc7a20ShakeDetector::FinalizeCandidate(uint32_t now_ms) {
    const uint32_t active_ms = last_strong_ms_ - candidate_start_ms_;
    const int shake_count =
        active_ms >= kSustainMs && strong_samples_ >= kMinStrongSamples &&
                reversal_count_ >= kMinReversals
            ? reversal_count_
            : 0;
    ResetCandidate();
    if (shake_count > 0) {
        has_triggered_ = true;
        last_trigger_ms_ = now_ms;
    }
    return shake_count;
}

Sc7a20MotionService& Sc7a20MotionService::GetInstance() {
    static Sc7a20MotionService instance;
    return instance;
}

bool Sc7a20MotionService::Start(i2c_master_bus_handle_t bus) {
    if (started_) return true;
    if (bus == nullptr) {
        ESP_LOGW(kTag, "motion service not started: null I2C bus");
        return false;
    }

    bus_ = bus;
    const BaseType_t created = xTaskCreate(TaskEntry, "sc7a20_motion", 4096,
                                           this, 1, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "failed to create motion task");
        return false;
    }
    started_ = true;
    return true;
}

void Sc7a20MotionService::SetSuspended(bool suspended) {
    suspended_.store(suspended);
    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
}

bool Sc7a20MotionService::ReadAcceleration(Sc7a20Sample* sample) const {
    if (sample == nullptr ||
        !acceleration_valid_.load(std::memory_order_acquire)) {
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t before =
            acceleration_sequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;
        sample->x_mg = acceleration_x_mg_.load(std::memory_order_relaxed);
        sample->y_mg = acceleration_y_mg_.load(std::memory_order_relaxed);
        sample->z_mg = acceleration_z_mg_.load(std::memory_order_relaxed);
        const uint32_t after =
            acceleration_sequence_.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U) return true;
    }
    return false;
}

bool Sc7a20MotionService::ReadTilt(Sc7a20Tilt* tilt) const {
    if (tilt == nullptr) return false;
    tilt->x_q10 = tilt_x_published_q10_.load(std::memory_order_acquire);
    tilt->y_q10 = tilt_y_published_q10_.load(std::memory_order_acquire);
    tilt->valid = tilt_valid_.load(std::memory_order_acquire);
    return tilt->valid;
}

void Sc7a20MotionService::TaskEntry(void* argument) {
    static_cast<Sc7a20MotionService*>(argument)->TaskMain();
}

void Sc7a20MotionService::TaskMain() {
    // The board starts this service only after LVGL exists. Give the rest of
    // application startup time to enter its main event loop before posting UI.
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (true) {
        if (suspended_.load()) {
            if (device_ != nullptr) {
                (void)WriteRegister(kCtrlReg1, kPowerDown);
                Disconnect();
            }
            detector_.Reset();
            ResetTilt();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!Connect()) {
            vTaskDelay(pdMS_TO_TICKS(kReconnectDelayMs));
            continue;
        }

        detector_.Reset();
        int consecutive_errors = 0;
        vTaskDelay(pdMS_TO_TICKS(100));
        while (device_ != nullptr) {
            Sc7a20Sample sample;
            if (!ReadSample(&sample)) {
                ++consecutive_errors;
                if (consecutive_errors >= kMaxConsecutiveReadErrors) {
                    ESP_LOGW(kTag, "sample reads failed %d times; reconnecting",
                             consecutive_errors);
                    Disconnect();
                    detector_.Reset();
                    break;
                }
            } else {
                consecutive_errors = 0;
                const uint32_t now_ms = NowMs();
                PublishAcceleration(sample);
                UpdateTilt(sample);
                ResetShakeActionCountIfExpired(now_ms);
                const int reversal_count = detector_.Update(sample, now_ms);
                if (reversal_count > 0) {
                    const int shake_action_count = RecordShakeAction(now_ms);
                    ESP_LOGI(kTag,
                             "shake action detected: %d reversals, action count=%d",
                             reversal_count, shake_action_count);
                    PostDizzyExpression(shake_action_count);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(kSamplePeriodMs));
        }
    }
}

bool Sc7a20MotionService::Connect() {
    if (device_ != nullptr) return true;

    static bool missing_logged = false;
    for (const uint8_t address : kAddresses) {
        if (i2c_master_probe(bus_, address, kProbeTimeoutMs) != ESP_OK) continue;

        i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = kI2cSpeedHz,
            .scl_wait_us = 0,
            .flags = {.disable_ack_check = 0},
        };
        if (i2c_master_bus_add_device(bus_, &config, &device_) != ESP_OK) {
            device_ = nullptr;
            continue;
        }

        uint8_t who_am_i = 0;
        if (!ReadRegister(kWhoAmIRegister, &who_am_i) ||
            who_am_i != kWhoAmIValue) {
            ESP_LOGW(kTag, "device @0x%02X has WHO_AM_I=0x%02X, expected 0x%02X",
                     address, who_am_i, kWhoAmIValue);
            Disconnect();
            continue;
        }

        address_ = address;
        if (!WriteRegister(kCtrlReg1, kPowerDown) ||
            !WriteRegister(kCtrlReg4, kBduHighResolution2g) ||
            !WriteRegister(kCtrlReg1, kOdr50HzAllAxes)) {
            ESP_LOGW(kTag, "configuration failed @0x%02X", address);
            Disconnect();
            continue;
        }

        missing_logged = false;
        ESP_LOGI(kTag, "SC7A20 online @0x%02X, 50 Hz, high-resolution +/-2 g",
                 address_);
        return true;
    }

    if (!missing_logged) {
        ESP_LOGW(kTag,
                 "SC7A20 not found at 0x18/0x19; motion expression disabled, "
                 "retrying every 10 s");
        missing_logged = true;
    }
    return false;
}

void Sc7a20MotionService::Disconnect() {
    if (device_ != nullptr) {
        (void)i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }
    address_ = 0;
    ResetAcceleration();
    ResetTilt();
}

bool Sc7a20MotionService::ReadRegister(uint8_t reg, uint8_t* value) {
    if (device_ == nullptr || value == nullptr) return false;
    return i2c_master_transmit_receive(device_, &reg, 1, value, 1,
                                       kI2cTimeoutMs) == ESP_OK;
}

bool Sc7a20MotionService::WriteRegister(uint8_t reg, uint8_t value) {
    if (device_ == nullptr) return false;
    const uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(device_, payload, sizeof(payload),
                               kI2cTimeoutMs) == ESP_OK;
}

bool Sc7a20MotionService::ReadSample(Sc7a20Sample* sample) {
    if (device_ == nullptr || sample == nullptr) return false;

    uint8_t start = kOutputStart | kAutoIncrement;
    uint8_t bytes[6] = {0};
    if (i2c_master_transmit_receive(device_, &start, 1, bytes, sizeof(bytes),
                                    kI2cTimeoutMs) != ESP_OK) {
        return false;
    }

    sample->x_mg = DecodeAxis(bytes[0], bytes[1]);
    sample->y_mg = DecodeAxis(bytes[2], bytes[3]);
    sample->z_mg = DecodeAxis(bytes[4], bytes[5]);
    return true;
}

void Sc7a20MotionService::PublishAcceleration(const Sc7a20Sample& sample) {
    acceleration_sequence_.fetch_add(1, std::memory_order_acq_rel);
    acceleration_x_mg_.store(sample.x_mg, std::memory_order_relaxed);
    acceleration_y_mg_.store(sample.y_mg, std::memory_order_relaxed);
    acceleration_z_mg_.store(sample.z_mg, std::memory_order_relaxed);
    acceleration_sequence_.fetch_add(1, std::memory_order_release);
    acceleration_valid_.store(true, std::memory_order_release);
}

void Sc7a20MotionService::ResetAcceleration() {
    acceleration_valid_.store(false, std::memory_order_release);
    acceleration_sequence_.fetch_add(1, std::memory_order_acq_rel);
    acceleration_x_mg_.store(0, std::memory_order_relaxed);
    acceleration_y_mg_.store(0, std::memory_order_relaxed);
    acceleration_z_mg_.store(0, std::memory_order_relaxed);
    acceleration_sequence_.fetch_add(1, std::memory_order_release);
}

void Sc7a20MotionService::UpdateTilt(const Sc7a20Sample& sample) {
    const int32_t target_x = NormalizeTiltQ10(sample.x_mg);
    const int32_t target_y = NormalizeTiltQ10(sample.y_mg);
    const int32_t delta_x = target_x - tilt_x_q10_;
    const int32_t delta_y = target_y - tilt_y_q10_;
    tilt_x_q10_ += std::abs(delta_x) <= kTiltFilterDivisor
                       ? delta_x
                       : delta_x / kTiltFilterDivisor;
    tilt_y_q10_ += std::abs(delta_y) <= kTiltFilterDivisor
                       ? delta_y
                       : delta_y / kTiltFilterDivisor;
    tilt_x_published_q10_.store(static_cast<int16_t>(tilt_x_q10_),
                                std::memory_order_release);
    tilt_y_published_q10_.store(static_cast<int16_t>(tilt_y_q10_),
                                std::memory_order_release);
    tilt_valid_.store(true, std::memory_order_release);
}

void Sc7a20MotionService::ResetTilt() {
    tilt_x_q10_ = 0;
    tilt_y_q10_ = 0;
    tilt_x_published_q10_.store(0, std::memory_order_release);
    tilt_y_published_q10_.store(0, std::memory_order_release);
    tilt_valid_.store(false, std::memory_order_release);
}

void Sc7a20MotionService::ResetShakeActionCountIfExpired(uint32_t now_ms) {
    if (shake_action_count_ <= 0 ||
        now_ms - last_shake_action_ms_ < kShakeCountResetMs) {
        return;
    }
    shake_action_count_ = 0;
    last_shake_action_ms_ = 0;
    ESP_LOGI(kTag, "shake action count reset after 5 minutes of inactivity");
}

int Sc7a20MotionService::RecordShakeAction(uint32_t now_ms) {
    ResetShakeActionCountIfExpired(now_ms);
    last_shake_action_ms_ = now_ms;
    return ++shake_action_count_;
}

void Sc7a20MotionService::PostDizzyExpression(int shake_action_count) {
    Application::GetInstance().Schedule([shake_action_count]() {
        // Standby is side-key-only: motion must neither wake the display nor
        // start an AI special interaction behind the black screen.
        if (agent_ui::StandbyView::IsActive()) {
            return;
        }
        agent_ui::IdlePower::Get().NotifyActivity();
        agent_ui::Runtime::Get().PlayDizzyExpression();
        Application::GetInstance().TriggerSpecialInteraction(
            SpecialInteraction::Dizzy, shake_action_count);
    });
}
