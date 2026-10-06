#pragma once

#include <cstdint>
#include <mutex>

#include "battery_snapshot.h"

// A coherent, bounded-age cache shared by battery consumers. Only the
// background sampler publishes; readers take a short lock while copying a
// small fixed-size snapshot and never touch I2C or trigger a probe.
class BatterySnapshotCache {
public:
    void Publish(const BatteryReading& reading, int64_t sampled_at_us);
    bool Read(int64_t now_us, uint32_t max_age_ms,
              BatterySnapshot& snapshot) const;

private:
    mutable std::mutex mutex_;
    BatterySnapshot snapshot_{};
};
