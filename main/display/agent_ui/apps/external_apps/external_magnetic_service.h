#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include <driver/i2c_master.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "metalio_app_api.h"

class MagneticService {
public:
    static MagneticService& Get();
    static MagneticService* Existing();

    // Starts a passive ID probe on the board-owned shared I2C bus.
    bool InitBus(i2c_master_bus_handle_t bus);
    bool Available() const;

    // Schedules lazy activation and copies the latest snapshot without I2C.
    int Read(void* owner, metalio_app_magnetic_sample_ex_t* sample);
    void SuspendOwner(void* owner);
    void UnloadOwner(void* owner);

private:
    MagneticService() = default;
    MagneticService(const MagneticService&) = delete;
    MagneticService& operator=(const MagneticService&) = delete;

    static void TaskEntry(void* argument);
    static void SampleTimerEntry(void* argument);
    void TaskMain();
    bool Probe();
    bool Configure();
    bool SuspendSensor();
    bool ReadRegister(uint8_t reg, uint8_t* value);
    bool ReadRegisters(uint8_t reg, uint8_t* values, size_t length);
    bool WriteRegister(uint8_t reg, uint8_t value);
    bool HasActiveOwners();
    void RemoveOwner(void* owner);
    void PublishSample(int16_t x, int16_t y, int16_t z, bool overflow);
    void PublishReadFailure();
    void InvalidateSample();
    void PublishOverflowFlag();

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    esp_timer_handle_t sample_timer_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> available_{false};
    std::atomic<bool> read_failed_{false};
    portMUX_TYPE owner_mux_ = portMUX_INITIALIZER_UNLOCKED;
    std::array<void*, 4> active_owners_{};
    portMUX_TYPE sample_mux_ = portMUX_INITIALIZER_UNLOCKED;
    metalio_app_magnetic_sample_ex_t latest_sample_{};
    bool sensor_running_ = false;
};
