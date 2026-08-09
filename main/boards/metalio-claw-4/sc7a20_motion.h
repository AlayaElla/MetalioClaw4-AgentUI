#pragma once

#include <cstdint>
#include <atomic>

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct Sc7a20Sample {
    int16_t x_mg = 0;
    int16_t y_mg = 0;
    int16_t z_mg = 0;
};

// A filtered board-relative tilt vector in Q10 ([-1000, 1000]). The motion
// task publishes this through atomics so the LVGL thread can sample it without
// touching the sensor or sharing a mutable C++ object.
struct Sc7a20Tilt {
    int16_t x_q10 = 0;
    int16_t y_q10 = 0;
    bool valid = false;
};

// Filters gravity out of SC7A20 samples and requires sustained, reversing
// motion. A single bump or a slow orientation change must not trigger dizzy.
class Sc7a20ShakeDetector {
public:
    // Returns the completed burst's direction-reversal count, or zero while
    // no qualifying shake burst has finished.
    int Update(const Sc7a20Sample& sample, uint32_t now_ms);
    void Reset();

private:
    struct Vector {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
    };

    void ResetCandidate();
    void StartCandidate(const Vector& dynamic, uint32_t now_ms);
    int FinalizeCandidate(uint32_t now_ms);

    bool gravity_initialized_ = false;
    int32_t gravity_q4_[3] = {0, 0, 0};

    bool candidate_active_ = false;
    uint32_t candidate_start_ms_ = 0;
    uint32_t last_strong_ms_ = 0;
    uint32_t last_reversal_ms_ = 0;
    int strong_samples_ = 0;
    int reversal_count_ = 0;
    Vector lobe_{};

    bool has_triggered_ = false;
    uint32_t last_trigger_ms_ = 0;
};

// Board-local SC7A20 service. It reuses the Metalio shared I2C bus, samples in
// a low-priority task, and posts a "dizzy" expression to the application loop.
class Sc7a20MotionService {
public:
    static Sc7a20MotionService& GetInstance();

    bool Start(i2c_master_bus_handle_t bus);
    void SetSuspended(bool suspended);
    bool ReadTilt(Sc7a20Tilt* tilt) const;

private:
    Sc7a20MotionService() = default;
    Sc7a20MotionService(const Sc7a20MotionService&) = delete;
    Sc7a20MotionService& operator=(const Sc7a20MotionService&) = delete;

    static void TaskEntry(void* argument);
    void TaskMain();

    bool Connect();
    void Disconnect();
    bool ReadRegister(uint8_t reg, uint8_t* value);
    bool WriteRegister(uint8_t reg, uint8_t value);
    bool ReadSample(Sc7a20Sample* sample);
    void UpdateTilt(const Sc7a20Sample& sample);
    void ResetTilt();
    void ResetShakeActionCountIfExpired(uint32_t now_ms);
    int RecordShakeAction(uint32_t now_ms);
    void PostDizzyExpression(int shake_action_count);

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    uint8_t address_ = 0;
    Sc7a20ShakeDetector detector_{};
    int shake_action_count_ = 0;
    uint32_t last_shake_action_ms_ = 0;
    int32_t tilt_x_q10_ = 0;
    int32_t tilt_y_q10_ = 0;
    std::atomic<int16_t> tilt_x_published_q10_{0};
    std::atomic<int16_t> tilt_y_published_q10_{0};
    std::atomic<bool> tilt_valid_{false};
    bool started_ = false;
    std::atomic<bool> suspended_{false};
    TaskHandle_t task_handle_ = nullptr;
};
