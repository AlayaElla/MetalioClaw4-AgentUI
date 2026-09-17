#pragma once

#include <cstdint>
#include <mutex>

// Serializes stack ownership changes with automatic connection timeout cleanup.
// A generation also invalidates waiters when settings releases and reacquires Wi-Fi.
class WifiConnectionOwnership {
public:
    static WifiConnectionOwnership& GetInstance() {
        static WifiConnectionOwnership instance;
        return instance;
    }

    template <typename Start>
    uint64_t StartAutomatic(Start start) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (manual_) return 0;
        const auto generation = ++generation_;
        start();
        return generation;
    }

    bool IsCurrent(uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        return !manual_ && generation != 0 && generation == generation_;
    }

    template <typename Stop>
    void StopAutomatic(uint64_t generation, Stop stop) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (manual_ || generation == 0 || generation != generation_) return;
        ++generation_;
        stop();
    }

    template <typename Takeover>
    void AcquireManual(Takeover takeover) {
        std::lock_guard<std::mutex> lock(mutex_);
        manual_ = true;
        ++generation_;
        takeover();
    }

    template <typename Restore>
    void ReleaseManual(Restore restore) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!manual_) return;
        restore();
        ++generation_;
        manual_ = false;
    }

private:
    std::mutex mutex_;
    uint64_t generation_ = 0;
    bool manual_ = false;
};
