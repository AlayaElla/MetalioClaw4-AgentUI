#pragma once

#include <cstdint>
#include <limits>

struct BatteryReading {
    int level = 0;
    bool charging = false;
    bool discharging = false;
    uint16_t voltage_mv = 0;
    int16_t current_ma = 0;
    bool voltage_valid = false;
    bool current_valid = false;
    bool current_sample_fresh = false;
    bool external_power = false;
    bool external_power_valid = false;
};

// Fixed-size, copyable battery state shared between the board and its
// background sampler. The snapshot deliberately contains freshness metadata
// so consumers can distinguish a retained value from a current measurement.
struct BatterySnapshot {
    int level = 0;
    bool charging = false;
    bool discharging = false;
    uint16_t voltage_mv = 0;
    int16_t current_ma = 0;
    bool voltage_valid = false;
    bool current_valid = false;
    bool current_sample_fresh = false;
    bool external_power = false;
    bool external_power_valid = false;
    bool has_data = false;
    bool fresh = false;
    uint32_t age_ms = std::numeric_limits<uint32_t>::max();
    uint32_t generation = 0;
    int64_t sampled_at_us = 0;
};
