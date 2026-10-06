#include "external_magnetic_service.h"

#include <array>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "qmc6309_math.h"

namespace {

constexpr char kTag[] = "MagneticService";
constexpr uint32_t kI2cSpeedHz = 100 * 1000;
constexpr int kI2cTimeoutMs = 50;
constexpr uint32_t kSamplePeriodUs = 5000;
constexpr uint32_t kIdlePollMs = 100;
std::atomic<MagneticService*> s_existing{nullptr};

}  // namespace

MagneticService& MagneticService::Get() {
    static MagneticService instance;
    s_existing.store(&instance, std::memory_order_release);
    return instance;
}

MagneticService* MagneticService::Existing() {
    return s_existing.load(std::memory_order_acquire);
}

bool MagneticService::InitBus(i2c_master_bus_handle_t bus) {
    if (bus == nullptr) return false;
    if (started_.load(std::memory_order_acquire)) return bus_ == bus;

    bus_ = bus;
    esp_timer_create_args_t timer_args = {
        .callback = SampleTimerEntry,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "qmc6309_sample",
    };
    esp_err_t error = esp_timer_create(&timer_args, &sample_timer_);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "sample timer create failed: %s", esp_err_to_name(error));
        sample_timer_ = nullptr;
        return false;
    }

    const BaseType_t created = xTaskCreate(TaskEntry, "qmc6309_service", 4096,
                                           this, 4, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "service task create failed");
        task_handle_ = nullptr;
        esp_timer_delete(sample_timer_);
        sample_timer_ = nullptr;
        return false;
    }
    started_.store(true, std::memory_order_release);
    return true;
}

bool MagneticService::Available() const {
    return available_.load(std::memory_order_acquire);
}

int MagneticService::Read(
    void* owner, metalio_app_magnetic_sample_ex_t* sample) {
    if (owner == nullptr || sample == nullptr) return -1;
    *sample = {};
    if (!Available()) return -2;

    bool registered = false;
    bool had_active_owner = false;
    portENTER_CRITICAL(&owner_mux_);
    size_t empty_slot = active_owners_.size();
    for (size_t index = 0; index < active_owners_.size(); ++index) {
        if (active_owners_[index] != nullptr) had_active_owner = true;
        if (active_owners_[index] == owner) {
            registered = true;
            break;
        }
        if (active_owners_[index] == nullptr && empty_slot == active_owners_.size()) {
            empty_slot = index;
        }
    }
    if (!registered && empty_slot < active_owners_.size()) {
        active_owners_[empty_slot] = owner;
        registered = true;
    }
    portEXIT_CRITICAL(&owner_mux_);

    if (!registered) return -2;
    if (!had_active_owner) InvalidateSample();
    if (task_handle_ != nullptr) xTaskNotifyGive(task_handle_);

    portENTER_CRITICAL(&sample_mux_);
    *sample = latest_sample_;
    portEXIT_CRITICAL(&sample_mux_);

    return read_failed_.load(std::memory_order_acquire) ? -2 : 0;
}

void MagneticService::SuspendOwner(void* owner) {
    RemoveOwner(owner);
}

void MagneticService::UnloadOwner(void* owner) {
    RemoveOwner(owner);
}

void MagneticService::RemoveOwner(void* owner) {
    if (owner == nullptr) return;
    bool any_active_owner = false;
    portENTER_CRITICAL(&owner_mux_);
    for (void*& active_owner : active_owners_) {
        if (active_owner == owner) active_owner = nullptr;
        if (active_owner != nullptr) any_active_owner = true;
    }
    portEXIT_CRITICAL(&owner_mux_);
    if (!any_active_owner) InvalidateSample();
    if (task_handle_ != nullptr) xTaskNotifyGive(task_handle_);
}

void MagneticService::TaskEntry(void* argument) {
    static_cast<MagneticService*>(argument)->TaskMain();
}

void MagneticService::SampleTimerEntry(void* argument) {
    auto* service = static_cast<MagneticService*>(argument);
    if (service->task_handle_ != nullptr) xTaskNotifyGive(service->task_handle_);
}

bool MagneticService::HasActiveOwners() {
    bool active = false;
    portENTER_CRITICAL(&owner_mux_);
    for (void* owner : active_owners_) {
        if (owner != nullptr) {
            active = true;
            break;
        }
    }
    portEXIT_CRITICAL(&owner_mux_);
    return active;
}

void MagneticService::TaskMain() {
    if (!Probe()) {
        ESP_LOGW(kTag, "QMC6309 probe failed; magnetometer capability disabled");
        if (sample_timer_ != nullptr) {
            (void)esp_timer_delete(sample_timer_);
            sample_timer_ = nullptr;
        }
        for (;;) (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    ESP_LOGI(kTag, "QMC6309 CHIP_ID=0x%02X at address 0x%02X",
             qmc6309::kChipId, qmc6309::kAddress);
    available_.store(true, std::memory_order_release);

    for (;;) {
        if (sensor_running_ && !HasActiveOwners()) {
            if (sample_timer_ != nullptr) {
                (void)esp_timer_stop(sample_timer_);
            }
            if (!SuspendSensor()) {
                ESP_LOGW(kTag, "sensor suspend write failed");
                PublishReadFailure();
            }
            sensor_running_ = false;
        }

        if (!sensor_running_ && HasActiveOwners()) {
            if (!Configure()) {
                ESP_LOGW(kTag, "sensor activation failed");
                PublishReadFailure();
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            if (!HasActiveOwners()) {
                if (!SuspendSensor()) PublishReadFailure();
                continue;
            }
            read_failed_.store(false, std::memory_order_release);
            const esp_err_t timer_error =
                esp_timer_start_periodic(sample_timer_, kSamplePeriodUs);
            if (timer_error != ESP_OK) {
                ESP_LOGE(kTag, "sample timer start failed: %s",
                         esp_err_to_name(timer_error));
                PublishReadFailure();
                (void)SuspendSensor();
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            sensor_running_ = true;
        }

        if (sensor_running_) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (sensor_running_ && HasActiveOwners()) {
                uint8_t status = 0;
                if (!ReadRegister(qmc6309::kStatusRegister, &status)) {
                    PublishReadFailure();
                    continue;
                }
                if (qmc6309::Overflow(status)) PublishOverflowFlag();
                if (!qmc6309::DataReady(status)) continue;

                std::array<uint8_t, 6> data{};
                if (!ReadRegisters(qmc6309::kOutputStartRegister, data.data(),
                                   data.size())) {
                    PublishReadFailure();
                    continue;
                }
                PublishSample(qmc6309::DecodeAxis(data[0], data[1]),
                              qmc6309::DecodeAxis(data[2], data[3]),
                              qmc6309::DecodeAxis(data[4], data[5]),
                              qmc6309::Overflow(status));
            }
        } else {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdlePollMs));
        }
    }
}

bool MagneticService::Probe() {
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = qmc6309::kAddress,
        .scl_speed_hz = kI2cSpeedHz,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    esp_err_t error = i2c_master_bus_add_device(bus_, &config, &device_);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "QMC6309 device registration failed: %s",
                 esp_err_to_name(error));
        device_ = nullptr;
        return false;
    }

    uint8_t id = 0;
    if (!ReadRegister(qmc6309::kChipIdRegister, &id)) {
        ESP_LOGW(kTag, "QMC6309 CHIP_ID read failed");
        (void)i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return false;
    }
    if (id != qmc6309::kChipId) {
        ESP_LOGW(kTag, "QMC6309 CHIP_ID=0x%02X (expected 0x%02X)", id,
                 qmc6309::kChipId);
        (void)i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return false;
    }

    // Power-on default is suspend. Explicitly keep it there without applying
    // sampling configuration until an App requests its first snapshot.
    if (!WriteRegister(qmc6309::kControl1Register, 0)) {
        ESP_LOGW(kTag, "could not confirm QMC6309 suspend after probe");
    }
    return true;
}

bool MagneticService::Configure() {
    if (!WriteRegister(qmc6309::kControl2Register,
                       qmc6309::kControl2SoftReset)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(25));
    if (!WriteRegister(qmc6309::kControl2Register,
                       qmc6309::kControl2Odr200HzRange32G)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    const uint8_t suspend = qmc6309::kControl1Base | qmc6309::kModeSuspend;
    const uint8_t normal = qmc6309::kControl1Base | qmc6309::kModeNormal;
    if (!WriteRegister(qmc6309::kControl1Register, suspend)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!WriteRegister(qmc6309::kControl1Register, normal)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!WriteRegister(qmc6309::kControl1Register, suspend)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!WriteRegister(qmc6309::kControl1Register, normal)) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

bool MagneticService::SuspendSensor() {
    return WriteRegister(qmc6309::kControl1Register,
                         qmc6309::kControl1Base | qmc6309::kModeSuspend);
}

bool MagneticService::ReadRegister(uint8_t reg, uint8_t* value) {
    if (device_ == nullptr || value == nullptr) return false;
    return i2c_master_transmit_receive(device_, &reg, 1, value, 1,
                                       kI2cTimeoutMs) == ESP_OK;
}

bool MagneticService::ReadRegisters(uint8_t reg, uint8_t* values,
                                   size_t length) {
    if (device_ == nullptr || values == nullptr || length == 0) return false;
    return i2c_master_transmit_receive(device_, &reg, 1, values, length,
                                       kI2cTimeoutMs) == ESP_OK;
}

bool MagneticService::WriteRegister(uint8_t reg, uint8_t value) {
    if (device_ == nullptr) return false;
    const uint8_t bytes[] = {reg, value};
    return i2c_master_transmit(device_, bytes, sizeof(bytes),
                               kI2cTimeoutMs) == ESP_OK;
}

void MagneticService::PublishSample(int16_t x, int16_t y, int16_t z,
                                    bool overflow) {
    const uint32_t timestamp_ms =
        static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    portENTER_CRITICAL(&sample_mux_);
    ++latest_sample_.sequence;
    latest_sample_.x_nanotesla = qmc6309::ToNanotesla(x);
    latest_sample_.y_nanotesla = qmc6309::ToNanotesla(y);
    latest_sample_.z_nanotesla = qmc6309::ToNanotesla(z);
    latest_sample_.timestamp_ms = timestamp_ms;
    latest_sample_.valid = 1;
    latest_sample_.overflow = overflow ? 1 : 0;
    latest_sample_.reserved[0] = 0;
    latest_sample_.reserved[1] = 0;
    portEXIT_CRITICAL(&sample_mux_);
    read_failed_.store(false, std::memory_order_release);
}

void MagneticService::PublishReadFailure() {
    portENTER_CRITICAL(&sample_mux_);
    latest_sample_.valid = 0;
    portEXIT_CRITICAL(&sample_mux_);
    read_failed_.store(true, std::memory_order_release);
}

void MagneticService::InvalidateSample() {
    portENTER_CRITICAL(&sample_mux_);
    latest_sample_.valid = 0;
    portEXIT_CRITICAL(&sample_mux_);
}

void MagneticService::PublishOverflowFlag() {
    portENTER_CRITICAL(&sample_mux_);
    latest_sample_.overflow = 1;
    portEXIT_CRITICAL(&sample_mux_);
}
