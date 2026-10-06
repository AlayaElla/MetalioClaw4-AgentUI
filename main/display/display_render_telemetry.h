#pragma once

#include <cstdint>

#include "lvgl.h"

namespace display_telemetry {

enum class Event {
    RefreshStart,
    RenderStart,
    FlushStart,
    FlushFinish,
    FlushWaitStart,
    FlushWaitFinish,
    RefreshReady,
};

struct Area {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = -1;
    int32_t y2 = -1;
};

struct WindowReport {
    const char* render_mode = nullptr;
    uint64_t rendered_frames = 0;
    uint64_t refresh_total_us = 0;
    uint64_t refresh_max_us = 0;
    uint64_t dirty_pixels = 0;
    uint64_t flush_callback_total_us = 0;
    uint64_t flush_wait_total_us = 0;
    uint64_t window_us = 0;
};

class DisplayRenderTelemetry {
public:
    using ClockCallback = int64_t (*)(void* context);
    using ReportCallback = void (*)(const WindowReport& report, void* context);

    explicit DisplayRenderTelemetry(const char* render_mode);
    DisplayRenderTelemetry(const DisplayRenderTelemetry&) = delete;
    DisplayRenderTelemetry& operator=(const DisplayRenderTelemetry&) = delete;
    DisplayRenderTelemetry(DisplayRenderTelemetry&&) = delete;
    DisplayRenderTelemetry& operator=(DisplayRenderTelemetry&&) = delete;

    // Attach exactly once on the LVGL owner thread. The telemetry object must
    // outlive callback delivery from the attached display; production stores it
    // inside the static display owner. Counter updates remain on that thread.
    // Repeated calls are ignored so the same callback is never registered twice.
    void Attach(lv_display_t* display, ClockCallback clock,
                ReportCallback report, void* context);

    // Exposed as a deterministic seam for host tests; production events arrive
    // through the LVGL callback registered by Attach().
    void BeginWindow(int64_t start_us);
    bool Record(Event event, int64_t now_us, const Area* dirty_area,
                WindowReport* completed_window);

private:
    static void EventCallback(lv_event_t* event);

    const char* render_mode_;
    ClockCallback clock_ = nullptr;
    ReportCallback report_ = nullptr;
    void* callback_context_ = nullptr;
    lv_display_t* attached_display_ = nullptr;
    int64_t window_start_us_ = 0;
    int64_t refresh_start_us_ = 0;
    int64_t flush_callback_start_us_ = 0;
    int64_t flush_wait_start_us_ = 0;
    uint64_t rendered_frames_ = 0;
    uint64_t refresh_total_us_ = 0;
    uint64_t refresh_max_us_ = 0;
    uint64_t dirty_pixels_ = 0;
    uint64_t flush_callback_total_us_ = 0;
    uint64_t flush_wait_total_us_ = 0;
    bool cycle_rendered_ = false;
};

}  // namespace display_telemetry
