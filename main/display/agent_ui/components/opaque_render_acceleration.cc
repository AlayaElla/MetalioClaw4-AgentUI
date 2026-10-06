#include "opaque_render_acceleration.h"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_private/esp_cache_private.h"
#include "soc/soc_caps.h"

#if CONFIG_SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#endif

namespace agent_ui::opaque_render_acceleration {

bool IsRegistrationBufferSupported(const lv_draw_buf_t* buffer) {
    return buffer != nullptr && buffer->data != nullptr && buffer->header.w != 0 &&
           buffer->header.h != 0 &&
           (buffer->header.cf == LV_COLOR_FORMAT_RGB565 ||
            buffer->header.cf == LV_COLOR_FORMAT_RGB888);
}

#if CONFIG_SOC_PPA_SUPPORTED
namespace {

constexpr char kTag[] = "ExpressionAccel";
constexpr size_t kMaxOpaqueRenderBuffers = 4;
constexpr size_t kOpaqueTagSize = 24;

struct KeyboardCopyStats {
    uint32_t hits = 0;
    uint32_t fallbacks = 0;
    uint32_t failures = 0;
    uint64_t total_us = 0;
    uint64_t max_us = 0;
} s_keyboard_stats;
constexpr char kKeyboardTag[] = "KeyboardPPA";
struct OpaqueRenderBuffer {
    const lv_draw_buf_t* buffer = nullptr;
    ppa_client_handle_t client = nullptr;
    char tag[kOpaqueTagSize] = {};
    bool keyboard = false;
    uint32_t hits = 0;
    uint32_t fallbacks = 0;
    uint32_t failures = 0;
    uint64_t total_us = 0;
    uint64_t max_us = 0;
    uint64_t cache_sync_bytes = 0;
};
std::array<OpaqueRenderBuffer, kMaxOpaqueRenderBuffers> s_opaque_buffers{};

OpaqueRenderBuffer* FindOpaqueRenderBuffer(const lv_draw_buf_t* buffer) {
    for (auto& entry : s_opaque_buffers) {
        if (buffer != nullptr && entry.buffer == buffer) return &entry;
    }
    return nullptr;
}

void LogKeyboardStats() {
    ESP_LOGI(kKeyboardTag, "copy hits=%" PRIu32 " fallback=%" PRIu32
                          " failed=%" PRIu32 " avg=%lluus max=%lluus",
             s_keyboard_stats.hits, s_keyboard_stats.fallbacks, s_keyboard_stats.failures,
             static_cast<unsigned long long>(s_keyboard_stats.hits == 0 ? 0 :
                 s_keyboard_stats.total_us / s_keyboard_stats.hits),
             static_cast<unsigned long long>(s_keyboard_stats.max_us));
}

void LogOpaqueStats(const OpaqueRenderBuffer* entry, const char* event) {
    if (entry == nullptr) return;
    ESP_LOGI(entry->tag, "%s hits=%" PRIu32 " fallback=%" PRIu32
                        " failed=%" PRIu32 " avg=%lluus max=%lluus sync=%lluB",
             event, entry->hits, entry->fallbacks, entry->failures,
             static_cast<unsigned long long>(entry->hits == 0 ? 0 :
                 entry->total_us / entry->hits),
             static_cast<unsigned long long>(entry->max_us),
             static_cast<unsigned long long>(entry->cache_sync_bytes));
}

bool OpaqueCopyFallback(OpaqueRenderBuffer* entry, const char* reason) {
    if (entry == nullptr) return false;
    ++entry->fallbacks;
    if (entry->keyboard) ++s_keyboard_stats.fallbacks;
    if (entry->fallbacks == 1 || entry->fallbacks % 100 == 0) {
        ESP_LOGW(entry->tag, "copy fallback: %s", reason);
        LogOpaqueStats(entry, "copy");
        if (entry->keyboard) LogKeyboardStats();
    }
    return false;
}

bool CopyOpaqueRenderBuffer(lv_draw_task_t* task, const lv_draw_sw_blend_dsc_t* descriptor,
                            lv_color_format_t format) {
    // Registration is limited to caller-owned snapshots with no tiling or transform.
    OpaqueRenderBuffer* entry = nullptr;
    for (auto& candidate : s_opaque_buffers) {
        if (candidate.buffer != nullptr && descriptor->src_buf == candidate.buffer->data) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) return false;
    const auto* source = entry->buffer;
    if (entry->client == nullptr) return OpaqueCopyFallback(entry, "no-client");
    auto* layer = task->target_layer;
    if (layer == nullptr || layer->draw_buf == nullptr || layer->color_format != format ||
        descriptor->src_color_format != format || source->header.cf != format ||
        descriptor->opa != LV_OPA_COVER || descriptor->mask_buf != nullptr ||
        descriptor->blend_mode != LV_BLEND_MODE_NORMAL || descriptor->src_area == nullptr ||
        descriptor->blend_area == nullptr) return OpaqueCopyFallback(entry, "unsupported-draw");

    const uint32_t pixel_bytes = format == LV_COLOR_FORMAT_RGB888 ? 3 : 2;
    const uint32_t source_stride = descriptor->src_stride != 0 ? descriptor->src_stride :
        lv_area_get_width(descriptor->src_area) * pixel_bytes;
    const uint32_t destination_stride = layer->draw_buf->header.stride;
    if (source_stride != source->header.stride || source_stride != source->header.w * pixel_bytes ||
        lv_area_get_width(descriptor->src_area) != source->header.w ||
        lv_area_get_height(descriptor->src_area) != source->header.h ||
        destination_stride != lv_area_get_width(&layer->buf_area) * pixel_bytes) {
        return OpaqueCopyFallback(entry, "stride-or-source-shape");
    }
    lv_area_t area;
    if (!lv_area_intersect(&area, descriptor->blend_area, &task->clip_area)) return true;
    const int32_t source_x = area.x1 - descriptor->src_area->x1;
    const int32_t source_y = area.y1 - descriptor->src_area->y1;
    const int32_t destination_x = area.x1 - layer->buf_area.x1;
    const int32_t destination_y = area.y1 - layer->buf_area.y1;
    const uint32_t width = lv_area_get_width(&area);
    const uint32_t height = lv_area_get_height(&area);
    if (source_x < 0 || source_y < 0 || destination_x < 0 || destination_y < 0 ||
        source_x + width > source->header.w || source_y + height > source->header.h ||
        area.x2 > layer->buf_area.x2 || area.y2 > layer->buf_area.y2 ||
        static_cast<size_t>(source_stride) * source->header.h > source->data_size) {
        return OpaqueCopyFallback(entry, "bounds");
    }
    auto* destination = layer->draw_buf->data;
    const size_t destination_bytes = static_cast<size_t>(destination_stride) *
                                     lv_area_get_height(&layer->buf_area);
    // Match the driver's platform alignment even if a future draw layer uses
    // internal RAM, whose own cache line can be smaller than the PPA's line.
    size_t alignment = 0;
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &alignment) != ESP_OK ||
        alignment == 0) return OpaqueCopyFallback(entry, "cache-alignment-query");
    const size_t output_size = (destination_bytes + alignment - 1) & ~(alignment - 1);
    if (destination == source->data || destination == nullptr ||
        reinterpret_cast<uintptr_t>(destination) % alignment != 0 ||
        output_size > layer->draw_buf->data_size) {
        return OpaqueCopyFallback(entry, "output-allocation");
    }

    const int64_t started_us = esp_timer_get_time();
    // The IDF SRM driver invalidates whole destination rows before DMA. For
    // clipped copies, write back those rows first to preserve CPU-drawn pixels
    // outside the copied rectangle, including neighbouring cache-line bytes.
    // Full-width copies overwrite every row byte, so only their unaligned
    // boundary lines need preserving; avoid writing back the entire keyboard.
    const uintptr_t row_start = reinterpret_cast<uintptr_t>(destination) +
                                static_cast<size_t>(destination_y) * destination_stride;
    const size_t row_bytes = static_cast<size_t>(height) * destination_stride;
    const size_t prefix = row_start % alignment;
    const size_t sync_bytes = (row_bytes + prefix + alignment - 1) & ~(alignment - 1);
    if (destination_x != 0 || width * pixel_bytes != destination_stride) {
        if (esp_cache_msync(reinterpret_cast<void*>(row_start - prefix), sync_bytes,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) {
            return OpaqueCopyFallback(entry, "cache-writeback");
        }
        entry->cache_sync_bytes += sync_bytes;
    } else {
        const uintptr_t first_line = row_start - prefix;
        const uintptr_t row_end = row_start + row_bytes;
        const uintptr_t last_line = row_end - row_end % alignment;
        if (prefix != 0 &&
            esp_cache_msync(reinterpret_cast<void*>(first_line), alignment,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) {
            return OpaqueCopyFallback(entry, "cache-writeback");
        }
        if (prefix != 0) entry->cache_sync_bytes += alignment;
        if (row_end % alignment != 0 && (prefix == 0 || last_line != first_line) &&
            esp_cache_msync(reinterpret_cast<void*>(last_line), alignment,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) {
            return OpaqueCopyFallback(entry, "cache-writeback");
        }
        if (row_end % alignment != 0 && (prefix == 0 || last_line != first_line)) {
            entry->cache_sync_bytes += alignment;
        }
    }
    const auto color_mode = format == LV_COLOR_FORMAT_RGB888 ?
        PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565;
    ppa_srm_oper_config_t config = {
        .in = {
            .buffer = source->data,
            .pic_w = source->header.w, .pic_h = source->header.h,
            .block_w = width, .block_h = height,
            .block_offset_x = static_cast<uint32_t>(source_x),
            .block_offset_y = static_cast<uint32_t>(source_y), .srm_cm = color_mode,
        },
        .out = {
            .buffer = destination,
            .buffer_size = static_cast<uint32_t>(output_size),
            .pic_w = destination_stride / pixel_bytes,
            .pic_h = static_cast<uint32_t>(lv_area_get_height(&layer->buf_area)),
            .block_offset_x = static_cast<uint32_t>(destination_x),
            .block_offset_y = static_cast<uint32_t>(destination_y), .srm_cm = color_mode,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    const esp_err_t result = ppa_do_scale_rotate_mirror(entry->client, &config);
    if (result != ESP_OK) {
        ++entry->failures;
        if (entry->keyboard) ++s_keyboard_stats.failures;
        return OpaqueCopyFallback(entry, esp_err_to_name(result));
    }
    const auto elapsed_us = static_cast<uint64_t>(esp_timer_get_time() - started_us);
    ++entry->hits;
    entry->total_us += elapsed_us;
    if (elapsed_us > entry->max_us) entry->max_us = elapsed_us;
    if (entry->hits == 1 || entry->hits % 300 == 0) LogOpaqueStats(entry, "copy");
    if (!entry->keyboard) return true;
    ++s_keyboard_stats.hits;
    s_keyboard_stats.total_us += elapsed_us;
    if (elapsed_us > s_keyboard_stats.max_us) s_keyboard_stats.max_us = elapsed_us;
    if (s_keyboard_stats.hits == 1 || s_keyboard_stats.hits % 300 == 0) LogKeyboardStats();
    return true;
}

void RegisterOpaqueInternal(const lv_draw_buf_t* buffer, const char* tag, bool keyboard) {
    OpaqueRenderBuffer* entry = FindOpaqueRenderBuffer(buffer);
    for (const auto& candidate : s_opaque_buffers) {
        if (candidate.buffer != nullptr && candidate.buffer != buffer &&
            candidate.buffer->data == buffer->data) {
            ESP_LOGW(tag != nullptr ? tag : kTag, "opaque render buffer data pointer already registered");
            return;
        }
    }
    if (entry == nullptr) {
        for (auto& candidate : s_opaque_buffers) {
            if (candidate.buffer == nullptr) { entry = &candidate; break; }
        }
    }
    if (entry == nullptr) {
        ESP_LOGW(tag != nullptr ? tag : kTag, "opaque render buffer registry full");
        return;
    }

    if (entry->client == nullptr) {
        const ppa_client_config_t config = {.oper_type = PPA_OPERATION_SRM};
        const esp_err_t result = ppa_register_client(&config, &entry->client);
        if (result != ESP_OK) {
            entry->client = nullptr;
            ESP_LOGW(tag != nullptr ? tag : kTag, "SRM client registration failed: %s",
                     esp_err_to_name(result));
            return;
        }
    }
    if (entry->buffer == nullptr) {
        entry->hits = 0;
        entry->fallbacks = 0;
        entry->failures = 0;
        entry->total_us = 0;
        entry->max_us = 0;
        entry->cache_sync_bytes = 0;
    }
    entry->buffer = buffer;
    entry->keyboard = keyboard;
    const char* source_tag = tag != nullptr ? tag : kTag;
    size_t i = 0;
    while (i + 1 < sizeof(entry->tag) && source_tag[i] != '\0') {
        entry->tag[i] = source_tag[i];
        ++i;
    }
    entry->tag[i] = '\0';
}

void UnregisterOpaqueInternal(const lv_draw_buf_t* buffer) {
    OpaqueRenderBuffer* entry = FindOpaqueRenderBuffer(buffer);
    if (entry == nullptr) return;
    if (entry->hits != 0 || entry->fallbacks != 0 || entry->failures != 0) {
        LogOpaqueStats(entry, "unregister");
    }
    if (entry->keyboard && (s_keyboard_stats.hits != 0 || s_keyboard_stats.fallbacks != 0)) {
        LogKeyboardStats();
    }
    if (entry->keyboard) s_keyboard_stats = {};
    if (entry->client != nullptr) {
        // SRM is blocking, so no operation can retain this buffer after return.
        const esp_err_t result = ppa_unregister_client(entry->client);
        if (result != ESP_OK) {
            ESP_LOGW(entry->tag, "SRM client release failed: %s", esp_err_to_name(result));
            // Stop matching the caller-owned memory even if the driver refuses
            // to release its client. Keep the client handle in this slot so a
            // later registration can reuse it without retaining the old buffer.
            entry->buffer = nullptr;
            entry->keyboard = false;
            entry->tag[0] = '\0';
            entry->hits = 0;
            entry->fallbacks = 0;
            entry->failures = 0;
            entry->total_us = 0;
            entry->max_us = 0;
            entry->cache_sync_bytes = 0;
            return;
        }
    }
    *entry = {};
}

}  // namespace

bool TryCopyRenderBuffer(lv_draw_task_t* task,
                         const lv_draw_sw_blend_dsc_t* descriptor,
                         lv_color_format_t format) {
    return CopyOpaqueRenderBuffer(task, descriptor, format);
}

void RegisterKeyboardBuffer(const lv_draw_buf_t* buffer) {
    RegisterOpaqueInternal(buffer, kKeyboardTag, true);
}

void UnregisterKeyboardBuffer(const lv_draw_buf_t* buffer) {
    OpaqueRenderBuffer* entry = FindOpaqueRenderBuffer(buffer);
    if (entry == nullptr || !entry->keyboard) return;
    UnregisterOpaqueInternal(buffer);
}

void RegisterBuffer(const lv_draw_buf_t* buffer, const char* log_tag) {
    RegisterOpaqueInternal(buffer, log_tag, false);
}

void UnregisterBuffer(const lv_draw_buf_t* buffer) {
    OpaqueRenderBuffer* entry = FindOpaqueRenderBuffer(buffer);
    if (entry == nullptr || entry->keyboard) return;
    UnregisterOpaqueInternal(buffer);
}

#else

bool TryCopyRenderBuffer(lv_draw_task_t* task,
                         const lv_draw_sw_blend_dsc_t* descriptor,
                         lv_color_format_t format) {
    (void)task;
    (void)descriptor;
    (void)format;
    return false;
}

void RegisterKeyboardBuffer(const lv_draw_buf_t* buffer) { (void)buffer; }
void UnregisterKeyboardBuffer(const lv_draw_buf_t* buffer) { (void)buffer; }
void RegisterBuffer(const lv_draw_buf_t* buffer, const char* log_tag) {
    (void)buffer;
    (void)log_tag;
}
void UnregisterBuffer(const lv_draw_buf_t* buffer) { (void)buffer; }

#endif

}  // namespace agent_ui::opaque_render_acceleration
