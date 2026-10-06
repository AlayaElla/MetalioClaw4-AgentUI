#include "battery_snapshot_cache.h"

#include <algorithm>
#include <limits>

void BatterySnapshotCache::Publish(const BatteryReading& reading,
                                   int64_t sampled_at_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t next_generation = snapshot_.generation + 1;
    snapshot_.level = reading.level;
    snapshot_.charging = reading.charging;
    snapshot_.discharging = reading.discharging;
    snapshot_.voltage_mv = reading.voltage_mv;
    snapshot_.current_ma = reading.current_ma;
    snapshot_.voltage_valid = reading.voltage_valid;
    snapshot_.current_valid = reading.current_valid;
    snapshot_.current_sample_fresh = reading.current_sample_fresh;
    snapshot_.external_power = reading.external_power;
    snapshot_.external_power_valid = reading.external_power_valid;
    snapshot_.has_data = reading.voltage_valid;
    snapshot_.fresh = reading.voltage_valid;
    snapshot_.age_ms = 0;
    snapshot_.generation = next_generation;
    snapshot_.sampled_at_us = sampled_at_us;
}

bool BatterySnapshotCache::Read(int64_t now_us, uint32_t max_age_ms,
                                BatterySnapshot& snapshot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = snapshot_;
    if (!snapshot.has_data) {
        snapshot.fresh = false;
        snapshot.age_ms = std::numeric_limits<uint32_t>::max();
        return false;
    }

    const int64_t elapsed_us = std::max<int64_t>(0, now_us - snapshot.sampled_at_us);
    const uint64_t age_ms = static_cast<uint64_t>(elapsed_us / 1000);
    snapshot.age_ms = static_cast<uint32_t>(std::min<uint64_t>(
        age_ms, std::numeric_limits<uint32_t>::max()));
    snapshot.fresh = age_ms <= max_age_ms;
    return true;
}
