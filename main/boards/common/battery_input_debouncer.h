#pragma once

#include <cstdint>

// Hysteresis for noisy charger VBUS status. Invalid reads neither advance nor
// reset a candidate, so transient bus failures cannot create charge edges.
class BatteryInputDebouncer {
public:
    bool Update(bool sample_valid, bool input_present) {
        if (!sample_valid) return false;
        if (!initialized_) {
            initialized_ = true;
            present_ = input_present;
            candidate_samples_ = 0;
            return true;
        }
        if (input_present == present_) {
            candidate_samples_ = 0;
            return false;
        }
        ++candidate_samples_;
        const uint8_t required_samples = input_present ? 2 : 8;
        if (candidate_samples_ < required_samples) return false;
        present_ = input_present;
        candidate_samples_ = 0;
        return true;
    }

    bool initialized() const { return initialized_; }
    bool present() const { return present_; }
    uint8_t candidate_samples() const { return candidate_samples_; }

private:
    bool initialized_ = false;
    bool present_ = false;
    uint8_t candidate_samples_ = 0;
};
