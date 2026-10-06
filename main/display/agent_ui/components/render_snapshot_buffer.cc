#include "render_snapshot_buffer.h"

#include <atomic>
#include <limits>

#include "expression_acceleration.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

namespace agent_ui {
namespace {

constexpr size_t kPpaBufferAlignment = 128;
constexpr size_t kAlignedAllocatorOverheadAllowance = 128;
constexpr char kTag[] = "RenderSnapshot";

std::atomic<size_t> g_current_charge_bytes{0};
std::atomic<size_t> g_peak_charge_bytes{0};
std::atomic<size_t> g_rejected_allocations{0};
std::atomic<size_t> g_allocation_failures{0};
std::atomic<size_t> g_reuse_hits{0};
#if defined(RENDER_SNAPSHOT_BUFFER_TESTING) && !defined(ESP_PLATFORM)
std::atomic<bool> g_fail_next_allocation_for_test{false};
#endif

size_t BudgetChargeFor(size_t payload_bytes) {
    constexpr size_t kAlignmentPadding = kPpaBufferAlignment - 1;
    if (payload_bytes == 0 ||
        payload_bytes > std::numeric_limits<size_t>::max() - kAlignmentPadding) {
        return 0;
    }
    const size_t aligned_bytes =
        (payload_bytes + kAlignmentPadding) & ~kAlignmentPadding;
    if (aligned_bytes > std::numeric_limits<size_t>::max() -
                            kAlignedAllocatorOverheadAllowance) {
        return 0;
    }
    return aligned_bytes + kAlignedAllocatorOverheadAllowance;
}

bool TryReserveCacheBytes(size_t charge_bytes) {
    size_t current = g_current_charge_bytes.load(std::memory_order_relaxed);
    for (;;) {
        if (charge_bytes == 0 ||
            charge_bytes > RenderSnapshotBuffer::kGlobalCacheBudgetBytes ||
            current > RenderSnapshotBuffer::kGlobalCacheBudgetBytes - charge_bytes) {
            g_rejected_allocations.fetch_add(1, std::memory_order_relaxed);
#if defined(ESP_PLATFORM)
            ESP_LOGW(kTag, "snapshot cache budget reject: requested=%u current=%u limit=%u",
                     static_cast<unsigned>(charge_bytes), static_cast<unsigned>(current),
                     static_cast<unsigned>(RenderSnapshotBuffer::kGlobalCacheBudgetBytes));
#endif
            return false;
        }
        const size_t next = current + charge_bytes;
        if (g_current_charge_bytes.compare_exchange_weak(
                current, next, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            size_t peak = g_peak_charge_bytes.load(std::memory_order_relaxed);
            while (peak < next && !g_peak_charge_bytes.compare_exchange_weak(
                       peak, next, std::memory_order_relaxed,
                       std::memory_order_relaxed)) {}
            return true;
        }
    }
}

void ReleaseCacheBytes(size_t charge_bytes) {
    if (charge_bytes != 0) {
        g_current_charge_bytes.fetch_sub(charge_bytes, std::memory_order_acq_rel);
    }
}

void RecordAllocationFailure() {
    g_allocation_failures.fetch_add(1, std::memory_order_relaxed);
}

void* AllocateBuffer(size_t bytes) {
#if defined(RENDER_SNAPSHOT_BUFFER_TESTING) && !defined(ESP_PLATFORM)
    if (g_fail_next_allocation_for_test.exchange(false,
                                                std::memory_order_acq_rel)) {
        return nullptr;
    }
#endif
#if defined(ESP_PLATFORM)
    return heap_caps_aligned_alloc(kPpaBufferAlignment, bytes,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return lv_malloc(bytes);
#endif
}

void FreeBuffer(void* memory) {
    if (memory == nullptr) return;
#if defined(ESP_PLATFORM)
    heap_caps_free(memory);
#else
    lv_free(memory);
#endif
}

}  // namespace

RenderSnapshotBuffer::~RenderSnapshotBuffer() { Release(); }

bool RenderSnapshotBuffer::Prepare(uint32_t width, uint32_t height,
                                   lv_color_format_t format) {
    Invalidate();
    if (width == 0 || height == 0 || format == LV_COLOR_FORMAT_UNKNOWN) {
        Release();
        return false;
    }

    const uint32_t stride = lv_draw_buf_width_to_stride(width, format);
    const uint8_t bits_per_pixel = lv_color_format_get_bpp(format);
    const uint64_t minimum_stride =
        (static_cast<uint64_t>(width) * bits_per_pixel + 7U) / 8U;
    if (bits_per_pixel == 0 || minimum_stride >
            std::numeric_limits<uint32_t>::max() ||
        stride == 0 || stride < minimum_stride ||
        static_cast<size_t>(height) >
            std::numeric_limits<size_t>::max() / static_cast<size_t>(stride)) {
        Release();
        return false;
    }
    const size_t bytes = static_cast<size_t>(stride) * height;
    // LVGL draw buffers use a 32-bit data_size on this target.
    if (bytes > std::numeric_limits<uint32_t>::max()) {
        Release();
        return false;
    }

    if (memory_ != nullptr && width_ == width && height_ == height &&
        stride_ == stride && format_ == format && bytes_ == bytes) {
        if (HasMatchingDescriptor(width, height, format, stride, bytes)) {
            g_reuse_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (lv_draw_buf_init(&buffer_, width, height, format, stride, memory_,
                             static_cast<uint32_t>(bytes)) != LV_RESULT_OK ||
            !HasMatchingDescriptor(width, height, format, stride, bytes)) {
            RecordAllocationFailure();
            Release();
            return false;
        }
        lv_draw_buf_set_flag(&buffer_, LV_IMAGE_FLAGS_MODIFIABLE);
        g_reuse_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    Release();
    const size_t budget_charge_bytes = BudgetChargeFor(bytes);
    if (!TryReserveCacheBytes(budget_charge_bytes)) return false;
    memory_ = AllocateBuffer(bytes);
    if (memory_ == nullptr) {
        ReleaseCacheBytes(budget_charge_bytes);
        RecordAllocationFailure();
        return false;
    }
    if (lv_draw_buf_init(&buffer_, width, height, format, stride, memory_,
                         static_cast<uint32_t>(bytes)) != LV_RESULT_OK) {
        FreeBuffer(memory_);
        memory_ = nullptr;
        ReleaseCacheBytes(budget_charge_bytes);
        RecordAllocationFailure();
        return false;
    }
    width_ = width;
    height_ = height;
    stride_ = stride;
    format_ = format;
    bytes_ = bytes;
    budget_charge_bytes_ = budget_charge_bytes;
    if (!HasMatchingDescriptor(width, height, format, stride, bytes)) {
        RecordAllocationFailure();
        Release();
        return false;
    }
    lv_draw_buf_set_flag(&buffer_, LV_IMAGE_FLAGS_MODIFIABLE);
    return true;
}

bool RenderSnapshotBuffer::Capture(lv_obj_t* source) {
    Invalidate();
#if LV_USE_SNAPSHOT
    if (source == nullptr || memory_ == nullptr) return false;
    const lv_result_t result =
        lv_snapshot_take_to_draw_buf(source, format_, &buffer_);
    if (result != LV_RESULT_OK ||
        !HasMatchingDescriptor(width_, height_, format_, stride_, bytes_)) {
        DropImageCaches();
        return false;
    }

    lv_draw_buf_flush_cache(&buffer_, nullptr);
    captured_ = true;
    return true;
#else
    (void)source;
    return false;
#endif
}

void RenderSnapshotBuffer::RegisterOpaque(const char* tag) {
    Unregister();
    if (!valid()) return;
    RegisterOpaqueRenderBuffer(&buffer_, tag);
    registration_ = Registration::kOpaque;
}

void RenderSnapshotBuffer::RegisterKeyboard() {
    Unregister();
    if (!valid()) return;
    RegisterKeyboardRenderBuffer(&buffer_);
    registration_ = Registration::kKeyboard;
}

void RenderSnapshotBuffer::Invalidate() {
    Unregister();
    captured_ = false;
    DropImageCaches();
}

void RenderSnapshotBuffer::Release() {
    Unregister();
    captured_ = false;
    const size_t budget_charge_bytes = budget_charge_bytes_;
    if (memory_ != nullptr) {
        DropImageCaches();
        FreeBuffer(memory_);
    }
    buffer_ = {};
    memory_ = nullptr;
    width_ = height_ = stride_ = 0;
    format_ = LV_COLOR_FORMAT_UNKNOWN;
    bytes_ = 0;
    budget_charge_bytes_ = 0;
    ReleaseCacheBytes(budget_charge_bytes);
}

void RenderSnapshotBuffer::Adopt(RenderSnapshotBuffer& candidate) {
    if (this == &candidate) return;
    candidate.Unregister();
    candidate.DropImageCaches();
    Release();

    buffer_ = candidate.buffer_;
    memory_ = candidate.memory_;
    width_ = candidate.width_;
    height_ = candidate.height_;
    stride_ = candidate.stride_;
    format_ = candidate.format_;
    bytes_ = candidate.bytes_;
    budget_charge_bytes_ = candidate.budget_charge_bytes_;
    captured_ = candidate.captured_;

    candidate.buffer_ = {};
    candidate.memory_ = nullptr;
    candidate.width_ = candidate.height_ = candidate.stride_ = 0;
    candidate.format_ = LV_COLOR_FORMAT_UNKNOWN;
    candidate.bytes_ = 0;
    candidate.budget_charge_bytes_ = 0;
    candidate.captured_ = false;
}

RenderSnapshotBuffer::CacheStats RenderSnapshotBuffer::GetCacheStats() {
    CacheStats stats{};
    stats.current_charge_bytes =
        g_current_charge_bytes.load(std::memory_order_relaxed);
    stats.peak_charge_bytes = g_peak_charge_bytes.load(std::memory_order_relaxed);
    stats.rejected_allocations =
        g_rejected_allocations.load(std::memory_order_relaxed);
    stats.allocation_failures =
        g_allocation_failures.load(std::memory_order_relaxed);
    stats.reuse_hits = g_reuse_hits.load(std::memory_order_relaxed);
    return stats;
}

#if defined(RENDER_SNAPSHOT_BUFFER_TESTING) && !defined(ESP_PLATFORM)
void RenderSnapshotBuffer::FailNextAllocationForTest() {
    g_fail_next_allocation_for_test.store(true, std::memory_order_release);
}
#endif

void RenderSnapshotBuffer::Unregister() {
    switch (registration_) {
        case Registration::kOpaque:
            UnregisterOpaqueRenderBuffer(&buffer_);
            break;
        case Registration::kKeyboard:
            UnregisterKeyboardRenderBuffer(&buffer_);
            break;
        case Registration::kNone:
            break;
    }
    registration_ = Registration::kNone;
}

void RenderSnapshotBuffer::DropImageCaches() {
    if (memory_ == nullptr) return;
    lv_image_cache_drop(&buffer_);
    lv_image_header_cache_drop(&buffer_);
}

bool RenderSnapshotBuffer::HasMatchingDescriptor(uint32_t width, uint32_t height,
                                                  lv_color_format_t format,
                                                  uint32_t stride,
                                                  size_t bytes) const {
    return buffer_.data == memory_ && buffer_.data_size >= bytes &&
           buffer_.header.w == width && buffer_.header.h == height &&
           buffer_.header.cf == format && buffer_.header.stride == stride;
}

}  // namespace agent_ui
