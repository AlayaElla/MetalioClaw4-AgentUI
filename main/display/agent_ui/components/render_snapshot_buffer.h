#pragma once

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace agent_ui {

// Owns one reusable LVGL snapshot allocation and its PPA registration. Owners
// keep capture timing, source-state guards, and draw policy in their own slice.
// All lifecycle, capture, and registration calls must run on the LVGL owner
// thread or under the existing LVGL lock. This helper retains no source objects
// or asynchronous callbacks.
class RenderSnapshotBuffer {
public:
    struct CacheStats {
        size_t current_charge_bytes = 0;
        size_t peak_charge_bytes = 0;
        size_t rejected_allocations = 0;
        size_t allocation_failures = 0;
        size_t reuse_hits = 0;
    };

    static constexpr size_t kGlobalCacheBudgetBytes = 3U * 1024U * 1024U;

    RenderSnapshotBuffer() = default;
    RenderSnapshotBuffer(const RenderSnapshotBuffer&) = delete;
    RenderSnapshotBuffer& operator=(const RenderSnapshotBuffer&) = delete;
    RenderSnapshotBuffer(RenderSnapshotBuffer&&) = delete;
    RenderSnapshotBuffer& operator=(RenderSnapshotBuffer&&) = delete;
    ~RenderSnapshotBuffer();

    bool Prepare(uint32_t width, uint32_t height, lv_color_format_t format);
    bool Capture(lv_obj_t* source);
    void RegisterOpaque(const char* tag);
    void RegisterKeyboard();
    void Invalidate();
    void Release();

    // Adopt pixels from an unregistered candidate while keeping this owner's
    // descriptor address stable for the acceleration registry.
    void Adopt(RenderSnapshotBuffer& candidate);

    bool valid() const { return memory_ != nullptr && captured_; }
    bool allocated() const { return memory_ != nullptr; }
    size_t bytes() const { return bytes_; }
    const lv_draw_buf_t* buffer() const { return valid() ? &buffer_ : nullptr; }
    static CacheStats GetCacheStats();
#if defined(RENDER_SNAPSHOT_BUFFER_TESTING) && !defined(ESP_PLATFORM)
    static void FailNextAllocationForTest();
#endif

private:
    enum class Registration : uint8_t { kNone, kOpaque, kKeyboard };

    void Unregister();
    void DropImageCaches();
    bool HasMatchingDescriptor(uint32_t width, uint32_t height,
                               lv_color_format_t format, uint32_t stride,
                               size_t bytes) const;

    lv_draw_buf_t buffer_{};
    void* memory_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t stride_ = 0;
    lv_color_format_t format_ = LV_COLOR_FORMAT_UNKNOWN;
    size_t bytes_ = 0;
    size_t budget_charge_bytes_ = 0;
    Registration registration_ = Registration::kNone;
    bool captured_ = false;
};

}  // namespace agent_ui
