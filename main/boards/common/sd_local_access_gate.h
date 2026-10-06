#pragma once

#include <mutex>

// Serializes local FatFs work against USB MSC ownership changes. The caller
// supplies a fresh state reader so an operation checks both before and after
// taking the lock. USB transition work may wait here only on its background
// worker; UI code must never take the transition lock.
class SdLocalAccessGate {
public:
    struct State {
        bool mounted = false;
        bool busy = false;
        bool exported = false;
    };

    using StateReader = State (*)(void* context);

    bool TryBegin(StateReader read_state, void* context) {
        if (read_state == nullptr || !IsAvailable(read_state(context))) {
            return false;
        }
        if (!mutex_.try_lock()) return false;
        if (!IsAvailable(read_state(context))) {
            mutex_.unlock();
            return false;
        }
        return true;
    }

    void End() { mutex_.unlock(); }

    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }

    SdLocalAccessGate(const SdLocalAccessGate&) = delete;
    SdLocalAccessGate& operator=(const SdLocalAccessGate&) = delete;

    SdLocalAccessGate() = default;

private:
    static bool IsAvailable(const State& state) {
        return state.mounted && !state.busy && !state.exported;
    }

    std::mutex mutex_;
};
