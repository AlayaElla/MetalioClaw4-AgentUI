#pragma once

#include <functional>

class UiDispatcher {
public:
    // Must be initialized once from the LVGL thread before worker tasks post work.
    static bool Init();

    // Non-blocking. Returns false when the dispatcher is unavailable or full.
    static bool Post(std::function<void()> callback);
};
