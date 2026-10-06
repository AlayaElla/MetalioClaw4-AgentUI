#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

class UiDispatcher {
public:
    enum class PostResult : uint8_t {
        Accepted,
        Replaced,
        Unavailable,
        Full,
        ByteLimit,
    };

    enum class SnapshotKey : uint8_t {
        CodexState = 1,
        CodexConnection = 2,
    };

    struct Diagnostics {
        size_t queued_items = 0;
        size_t queued_bytes = 0;
        size_t high_water_items = 0;
        size_t high_water_bytes = 0;
        uint64_t accepted = 0;
        uint64_t rejected = 0;
        uint64_t replaced = 0;
    };

    using WakeCallback = void (*)();

    // Install before Init when the display owner provides an event wake and a
    // per-pass DrainPending callback. The fallback keeps the periodic LVGL timer.
    static bool ConfigureExecutor(WakeCallback wake_callback);

    // In the fallback configuration, Init must run while LVGL is locked.
    static bool Init();

    // Non-blocking. The original overload remains for small state updates.
    static bool Post(std::function<void()> callback);

    // Retained bytes account for owned payload memory captured by the callback.
    // Use this for messages whose queued storage is larger than the closure.
    static PostResult PostBounded(std::function<void()> callback,
                                  size_t retained_bytes);

    // Explicitly replaceable complete state snapshots have closed, named keys.
    // A replacement is moved to the tail so its order relative to commands and
    // acknowledgements remains consistent with its arrival time.
    static PostResult PostLatest(SnapshotKey key,
                                 std::function<void()> callback,
                                 size_t retained_bytes);

    static Diagnostics GetDiagnostics();

    // Called only by the configured display-owner hook while the display lock
    // is held. Count and time limits are checked between callbacks; a single
    // callback may run longer than the time budget. Wakes another pass if work
    // remains.
    static bool DrainPending();
};
