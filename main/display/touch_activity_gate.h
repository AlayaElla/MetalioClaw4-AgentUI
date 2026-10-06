#pragma once

#include <cstdint>

// Only coalesce idle/performance side effects. LVGL still receives every touch
// coordinate and press/release sample at its original input cadence.
class TouchActivityGate {
public:
    static constexpr uint32_t kHeartbeatMs = 100;

    bool ShouldNotify(bool pressed, uint32_t now_ms) {
        if (!pressed) {
            was_pressed_ = false;
            return false;
        }
        const bool notify = !was_pressed_ || now_ms - last_activity_ms_ >= kHeartbeatMs;
        was_pressed_ = true;
        if (notify) last_activity_ms_ = now_ms;
        return notify;
    }
    void Reset() { was_pressed_ = false; }

private:
    bool was_pressed_ = false;
    uint32_t last_activity_ms_ = 0;
};
