#include "display_render_telemetry.h"

namespace display_telemetry {
namespace {

constexpr int64_t kReportWindowUs = 3000000;

}  // namespace

DisplayRenderTelemetry::DisplayRenderTelemetry(const char* render_mode)
    : render_mode_(render_mode) {}

void DisplayRenderTelemetry::Attach(lv_display_t* display, ClockCallback clock,
                                    ReportCallback report, void* context) {
    if (attached_display_ != nullptr || display == nullptr || clock == nullptr ||
        report == nullptr) {
        return;
    }
    clock_ = clock;
    report_ = report;
    callback_context_ = context;
    BeginWindow(clock_(callback_context_));
    lv_display_add_event_cb(display, EventCallback, LV_EVENT_ALL, this);
    attached_display_ = display;
}

void DisplayRenderTelemetry::BeginWindow(int64_t start_us) {
    window_start_us_ = start_us;
    refresh_start_us_ = 0;
    flush_callback_start_us_ = 0;
    flush_wait_start_us_ = 0;
    rendered_frames_ = 0;
    refresh_total_us_ = 0;
    refresh_max_us_ = 0;
    dirty_pixels_ = 0;
    flush_callback_total_us_ = 0;
    flush_wait_total_us_ = 0;
    cycle_rendered_ = false;
}

bool DisplayRenderTelemetry::Record(Event event, int64_t now_us,
                                    const Area* dirty_area,
                                    WindowReport* completed_window) {
    switch (event) {
        case Event::RefreshStart:
            refresh_start_us_ = now_us;
            cycle_rendered_ = false;
            break;
        case Event::RenderStart:
            cycle_rendered_ = true;
            break;
        case Event::FlushStart:
            flush_callback_start_us_ = now_us;
            if (dirty_area != nullptr && dirty_area->x2 >= dirty_area->x1 &&
                dirty_area->y2 >= dirty_area->y1) {
                dirty_pixels_ +=
                    static_cast<uint64_t>(dirty_area->x2 - dirty_area->x1 + 1) *
                    static_cast<uint64_t>(dirty_area->y2 - dirty_area->y1 + 1);
            }
            break;
        case Event::FlushFinish:
            if (flush_callback_start_us_ != 0) {
                flush_callback_total_us_ += now_us - flush_callback_start_us_;
                flush_callback_start_us_ = 0;
            }
            break;
        case Event::FlushWaitStart:
            flush_wait_start_us_ = now_us;
            break;
        case Event::FlushWaitFinish:
            if (flush_wait_start_us_ != 0) {
                flush_wait_total_us_ += now_us - flush_wait_start_us_;
                flush_wait_start_us_ = 0;
            }
            break;
        case Event::RefreshReady:
            if (cycle_rendered_ && refresh_start_us_ != 0) {
                const uint64_t refresh_us =
                    static_cast<uint64_t>(now_us - refresh_start_us_);
                ++rendered_frames_;
                refresh_total_us_ += refresh_us;
                if (refresh_us > refresh_max_us_) refresh_max_us_ = refresh_us;
            }
            refresh_start_us_ = 0;
            cycle_rendered_ = false;

            // Idle refresh-ready events may close a window, but never emit an
            // empty report or advance the window start without rendered frames.
            if (now_us - window_start_us_ >= kReportWindowUs &&
                rendered_frames_ != 0) {
                if (completed_window != nullptr) {
                    completed_window->render_mode = render_mode_;
                    completed_window->rendered_frames = rendered_frames_;
                    completed_window->refresh_total_us = refresh_total_us_;
                    completed_window->refresh_max_us = refresh_max_us_;
                    completed_window->dirty_pixels = dirty_pixels_;
                    completed_window->flush_callback_total_us =
                        flush_callback_total_us_;
                    completed_window->flush_wait_total_us = flush_wait_total_us_;
                    completed_window->window_us =
                        static_cast<uint64_t>(now_us - window_start_us_);
                }
                window_start_us_ = now_us;
                rendered_frames_ = 0;
                refresh_total_us_ = 0;
                refresh_max_us_ = 0;
                dirty_pixels_ = 0;
                flush_callback_total_us_ = 0;
                flush_wait_total_us_ = 0;
                return true;
            }
            break;
    }
    return false;
}

void DisplayRenderTelemetry::EventCallback(lv_event_t* event) {
    auto* self = static_cast<DisplayRenderTelemetry*>(lv_event_get_user_data(event));
    if (self == nullptr || self->clock_ == nullptr) return;

    Event telemetry_event;
    switch (lv_event_get_code(event)) {
        case LV_EVENT_REFR_START: telemetry_event = Event::RefreshStart; break;
        case LV_EVENT_RENDER_START: telemetry_event = Event::RenderStart; break;
        case LV_EVENT_FLUSH_START: telemetry_event = Event::FlushStart; break;
        case LV_EVENT_FLUSH_FINISH: telemetry_event = Event::FlushFinish; break;
        case LV_EVENT_FLUSH_WAIT_START: telemetry_event = Event::FlushWaitStart; break;
        case LV_EVENT_FLUSH_WAIT_FINISH: telemetry_event = Event::FlushWaitFinish; break;
        case LV_EVENT_REFR_READY: telemetry_event = Event::RefreshReady; break;
        default: return;
    }

    Area area{};
    const Area* area_ptr = nullptr;
    if (telemetry_event == Event::FlushStart) {
        const auto* lv_area = static_cast<const lv_area_t*>(lv_event_get_param(event));
        if (lv_area != nullptr) {
            area = {lv_area->x1, lv_area->y1, lv_area->x2, lv_area->y2};
            area_ptr = &area;
        }
    }

    WindowReport report{};
    if (self->Record(telemetry_event, self->clock_(self->callback_context_),
                     area_ptr, &report) && self->report_ != nullptr) {
        self->report_(report, self->callback_context_);
    }
}

}  // namespace display_telemetry
