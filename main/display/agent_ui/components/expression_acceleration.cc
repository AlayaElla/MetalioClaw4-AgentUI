#include "expression_acceleration.h"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_private/esp_cache_private.h"
#include "soc/soc_caps.h"

#include "lvgl_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include "src/draw/sw/lv_draw_sw.h"

#if CONFIG_SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#endif

namespace agent_ui {
namespace {

constexpr char kTag[] = "ExpressionAccel";
constexpr uint32_t kMinPpaA8Pixels = 4096;
constexpr uint32_t kPpaHitLogInterval = 3000;
constexpr size_t kMaxExpressionA8Buffers = 4;

#if CONFIG_SOC_PPA_SUPPORTED

ppa_client_handle_t s_blend_handle = nullptr;
lv_draw_sw_blend_handler_t s_previous_rgb565_handler = nullptr;
lv_draw_sw_blend_handler_t s_previous_rgb888_handler = nullptr;
bool s_registered = false;
uint32_t s_ppa_hits = 0;
uint32_t s_ppa_fallbacks = 0;
uint32_t s_ppa_failures = 0;
uint32_t s_expression_ppa_hits = 0;

ppa_client_handle_t s_keyboard_copy_handle = nullptr;
const lv_draw_buf_t* s_keyboard_buffer = nullptr;
struct KeyboardCopyStats {
    uint32_t hits = 0;
    uint32_t fallbacks = 0;
    uint32_t failures = 0;
    uint64_t total_us = 0;
    uint64_t max_us = 0;
} s_keyboard_stats;
constexpr char kKeyboardTag[] = "KeyboardPPA";

struct ExpressionBufferRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;
};

std::array<ExpressionBufferRange, kMaxExpressionA8Buffers>
    s_expression_buffers{};

bool IsExpressionA8Buffer(const void* address) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    for (const auto& range : s_expression_buffers) {
        if (range.begin != 0 && value >= range.begin && value < range.end) {
            return true;
        }
    }
    return false;
}

size_t CacheAlignment(const void* address) {
    if (address == nullptr) return 0;
    size_t alignment = 0;
    const uint32_t caps = esp_ptr_external_ram(address)
                              ? MALLOC_CAP_SPIRAM
                              : (esp_ptr_internal(address) ? MALLOC_CAP_INTERNAL : 0);
    if (caps == 0 || esp_cache_get_alignment(caps, &alignment) != ESP_OK) return 0;
    return alignment;
}

bool IsCacheAligned(const void* address) {
    const size_t alignment = CacheAlignment(address);
    return alignment == 0 ||
           (reinterpret_cast<uintptr_t>(address) & (alignment - 1)) == 0;
}

size_t AlignSize(const void* address, size_t size) {
    const size_t alignment = CacheAlignment(address);
    return alignment == 0 ? size : ((size + alignment - 1) & ~(alignment - 1));
}

void LogKeyboardStats() {
    ESP_LOGI(kKeyboardTag, "copy hits=%" PRIu32 " fallback=%" PRIu32
                          " failed=%" PRIu32 " avg=%lluus max=%lluus",
             s_keyboard_stats.hits, s_keyboard_stats.fallbacks, s_keyboard_stats.failures,
             static_cast<unsigned long long>(s_keyboard_stats.hits == 0 ? 0 :
                 s_keyboard_stats.total_us / s_keyboard_stats.hits),
             static_cast<unsigned long long>(s_keyboard_stats.max_us));
}

bool KeyboardCopyFallback(const char* reason) {
    ++s_keyboard_stats.fallbacks;
    if (s_keyboard_stats.fallbacks == 1 || s_keyboard_stats.fallbacks % 100 == 0) {
        ESP_LOGW(kKeyboardTag, "copy fallback: %s", reason);
        LogKeyboardStats();
    }
    return false;
}

bool CopyKeyboardCache(lv_draw_task_t* task, const lv_draw_sw_blend_dsc_t* descriptor,
                       lv_color_format_t format) {
    // No general image fast path: private keyboard ownership guarantees this
    // source is never tiled or transformed. All other images use the adapter.
    const auto* source = s_keyboard_buffer;
    if (source == nullptr || descriptor->src_buf != source->data) return false;
    if (s_keyboard_copy_handle == nullptr) return KeyboardCopyFallback("no-client");
    auto* layer = task->target_layer;
    if (layer == nullptr || layer->draw_buf == nullptr || layer->color_format != format ||
        descriptor->src_color_format != format || source->header.cf != format ||
        descriptor->opa != LV_OPA_COVER || descriptor->mask_buf != nullptr ||
        descriptor->blend_mode != LV_BLEND_MODE_NORMAL || descriptor->src_area == nullptr ||
        descriptor->blend_area == nullptr) return KeyboardCopyFallback("unsupported-draw");

    const uint32_t pixel_bytes = format == LV_COLOR_FORMAT_RGB888 ? 3 : 2;
    const uint32_t source_stride = descriptor->src_stride != 0 ? descriptor->src_stride :
        lv_area_get_width(descriptor->src_area) * pixel_bytes;
    const uint32_t destination_stride = layer->draw_buf->header.stride;
    if (source_stride != source->header.stride || source_stride != source->header.w * pixel_bytes ||
        lv_area_get_width(descriptor->src_area) != source->header.w ||
        lv_area_get_height(descriptor->src_area) != source->header.h ||
        destination_stride % pixel_bytes != 0 ||
        destination_stride < lv_area_get_width(&layer->buf_area) * pixel_bytes) {
        return KeyboardCopyFallback("stride-or-source-shape");
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
        return KeyboardCopyFallback("bounds");
    }
    auto* destination = layer->draw_buf->data;
    const size_t destination_bytes = static_cast<size_t>(destination_stride) *
                                     lv_area_get_height(&layer->buf_area);
    // Match the driver's platform alignment even if a future draw layer uses
    // internal RAM, whose own cache line can be smaller than the PPA's line.
    size_t alignment = 0;
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &alignment) != ESP_OK ||
        alignment == 0) return KeyboardCopyFallback("cache-alignment-query");
    const size_t output_size = (destination_bytes + alignment - 1) & ~(alignment - 1);
    if (destination == source->data || destination == nullptr ||
        reinterpret_cast<uintptr_t>(destination) % alignment != 0 ||
        output_size > layer->draw_buf->data_size) {
        return KeyboardCopyFallback("output-allocation");
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
            return KeyboardCopyFallback("cache-writeback");
        }
    } else {
        const uintptr_t first_line = row_start - prefix;
        const uintptr_t row_end = row_start + row_bytes;
        const uintptr_t last_line = row_end - row_end % alignment;
        if (prefix != 0 &&
            esp_cache_msync(reinterpret_cast<void*>(first_line), alignment,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) {
            return KeyboardCopyFallback("cache-writeback");
        }
        if (row_end % alignment != 0 && (prefix == 0 || last_line != first_line) &&
            esp_cache_msync(reinterpret_cast<void*>(last_line), alignment,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) {
            return KeyboardCopyFallback("cache-writeback");
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
    const esp_err_t result = ppa_do_scale_rotate_mirror(s_keyboard_copy_handle, &config);
    if (result != ESP_OK) {
        ++s_keyboard_stats.failures;
        return KeyboardCopyFallback(esp_err_to_name(result));
    }
    ++s_keyboard_stats.hits;
    const auto elapsed_us = static_cast<uint64_t>(esp_timer_get_time() - started_us);
    s_keyboard_stats.total_us += elapsed_us;
    if (elapsed_us > s_keyboard_stats.max_us) s_keyboard_stats.max_us = elapsed_us;
    if (s_keyboard_stats.hits == 1 || s_keyboard_stats.hits % 300 == 0) LogKeyboardStats();
    return true;
}

void DelegateBlend(lv_draw_task_t* task, const lv_draw_sw_blend_dsc_t* descriptor,
                   lv_color_format_t destination_format,
                   lv_draw_sw_blend_handler_t handler) {
    if (handler != nullptr) {
        handler(task, descriptor);
        return;
    }

    // LVGL 9.3 does not expose a public `lv_draw_sw_blend_default` helper.
    // Temporarily remove this custom handler so the public dispatcher can run
    // its built-in software path, then restore the handler for later frames.
    lv_draw_sw_blend_handler_t registered =
        lv_draw_sw_get_blend_handler(destination_format);
    if (registered != nullptr && !lv_draw_sw_unregister_blend_handler(destination_format)) {
        return;
    }
    lv_draw_sw_blend(task, descriptor);
    if (registered != nullptr) {
        lv_draw_sw_custom_blend_handler_t restore = {
            .dest_cf = destination_format,
            .handler = registered,
        };
        lv_draw_sw_register_blend_handler(&restore);
    }
}

void LogFallback(const char* reason) {
    ++s_ppa_fallbacks;
    if (s_ppa_fallbacks == 1U || (s_ppa_fallbacks % 100U) == 0U) {
        ESP_LOGW(kTag, "PPA A8 fallback (%s): hits=%" PRIu32
                       " fallback=%" PRIu32 " failed=%" PRIu32,
                 reason, s_ppa_hits, s_ppa_fallbacks, s_ppa_failures);
    }
}

void BlendA8Mask(lv_draw_task_t* task, const lv_draw_sw_blend_dsc_t* descriptor,
                 lv_color_format_t destination_format,
                 ppa_blend_color_mode_t ppa_color_mode,
                 uint32_t bytes_per_pixel,
                 lv_draw_sw_blend_handler_t fallback_handler) {
    if (CopyKeyboardCache(task, descriptor, destination_format)) return;
    lv_layer_t* layer = task->target_layer;
    if (layer == nullptr || layer->draw_buf == nullptr ||
        layer->color_format != destination_format ||
        descriptor->src_buf != nullptr || descriptor->mask_buf == nullptr ||
        descriptor->mask_area == nullptr || descriptor->blend_mode != LV_BLEND_MODE_NORMAL) {
        DelegateBlend(task, descriptor, destination_format, fallback_handler);
        return;
    }

    lv_area_t block_area;
    if (!lv_area_intersect(&block_area, descriptor->blend_area, &task->clip_area)) return;

    const int32_t block_width = lv_area_get_width(&block_area);
    const int32_t block_height = lv_area_get_height(&block_area);
    if (block_width <= 0 || block_height <= 0) return;

    const bool expression_mask = IsExpressionA8Buffer(descriptor->mask_buf);
    // PPA setup and cache synchronization cost more than software blending for
    // small masks such as font glyphs. Programmatic expression buffers are
    // explicitly registered and always use PPA, including very small dirty
    // regions; unrelated A8 masks retain the threshold.
    if (static_cast<uint32_t>(block_width) * static_cast<uint32_t>(block_height) <
            kMinPpaA8Pixels &&
        !expression_mask) {
        DelegateBlend(task, descriptor, destination_format, fallback_handler);
        return;
    }

    const int32_t mask_offset_x = block_area.x1 - descriptor->mask_area->x1;
    const int32_t mask_offset_y = block_area.y1 - descriptor->mask_area->y1;
    const int32_t destination_offset_x = block_area.x1 - layer->buf_area.x1;
    const int32_t destination_offset_y = block_area.y1 - layer->buf_area.y1;
    const uint32_t mask_stride = descriptor->mask_stride != 0
                                     ? descriptor->mask_stride
                                     : lv_area_get_width(descriptor->mask_area);
    const uint32_t destination_stride = layer->draw_buf->header.stride;
    const uint32_t destination_width = destination_stride / bytes_per_pixel;
    const uint32_t destination_height = lv_area_get_height(&layer->buf_area);
    void* destination = layer->draw_buf->data;

    const char* fallback_reason = nullptr;
    if (mask_offset_x < 0 || mask_offset_y < 0 ||
               destination_offset_x < 0 || destination_offset_y < 0) {
        fallback_reason = "negative-offset";
    } else if (static_cast<uint32_t>(mask_offset_x + block_width) > mask_stride ||
               static_cast<uint32_t>(destination_offset_x + block_width) > destination_width) {
        fallback_reason = "row-bounds";
    } else if (destination == nullptr) {
        fallback_reason = "null-destination";
    } else if (!IsCacheAligned(destination)) {
        fallback_reason = "output-cache-alignment";
    }
    if (fallback_reason != nullptr) {
        LogFallback(fallback_reason);
        DelegateBlend(task, descriptor, destination_format, fallback_handler);
        return;
    }

    const uint32_t mask_height = lv_area_get_height(descriptor->mask_area);
    const lv_color32_t color = lv_color_to_32(descriptor->color, LV_OPA_COVER);
    ppa_blend_oper_config_t config = {
        .in_bg = {
            .buffer = destination,
            .pic_w = destination_width,
            .pic_h = destination_height,
            .block_w = static_cast<uint32_t>(block_width),
            .block_h = static_cast<uint32_t>(block_height),
            .block_offset_x = static_cast<uint32_t>(destination_offset_x),
            .block_offset_y = static_cast<uint32_t>(destination_offset_y),
            .blend_cm = ppa_color_mode,
        },
        .in_fg = {
            .buffer = descriptor->mask_buf,
            .pic_w = mask_stride,
            .pic_h = mask_height,
            .block_w = static_cast<uint32_t>(block_width),
            .block_h = static_cast<uint32_t>(block_height),
            .block_offset_x = static_cast<uint32_t>(mask_offset_x),
            .block_offset_y = static_cast<uint32_t>(mask_offset_y),
            .blend_cm = PPA_BLEND_COLOR_MODE_A8,
        },
        .out = {
            .buffer = destination,
            .buffer_size = AlignSize(
                destination, static_cast<size_t>(destination_stride) * destination_height),
            .pic_w = destination_width,
            .pic_h = destination_height,
            .block_offset_x = static_cast<uint32_t>(destination_offset_x),
            .block_offset_y = static_cast<uint32_t>(destination_offset_y),
            .blend_cm = ppa_color_mode,
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = LV_OPA_COVER,
        .fg_alpha_update_mode = descriptor->opa >= LV_OPA_MAX
                                    ? PPA_ALPHA_NO_CHANGE
                                    : PPA_ALPHA_SCALE,
        .fg_alpha_scale_ratio = static_cast<float>(descriptor->opa) /
                                static_cast<float>(LV_OPA_COVER),
        .fg_fix_rgb_val = {
            .b = color.blue,
            .g = color.green,
            .r = color.red,
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    const esp_err_t result = ppa_do_blend(s_blend_handle, &config);
    if (result != ESP_OK) {
        ++s_ppa_failures;
        LogFallback("ppa-error");
        ESP_LOGW(kTag, "PPA A8 blend failed: %s", esp_err_to_name(result));
        DelegateBlend(task, descriptor, destination_format, fallback_handler);
        return;
    }
    ++s_ppa_hits;
    if (expression_mask) ++s_expression_ppa_hits;
    if ((s_ppa_hits % kPpaHitLogInterval) == 0U) {
        ESP_LOGI(kTag, "PPA A8 hits=%" PRIu32 " fallback=%" PRIu32
                       " failed=%" PRIu32 " expression=%" PRIu32,
                 s_ppa_hits, s_ppa_fallbacks, s_ppa_failures,
                 s_expression_ppa_hits);
    }
}

void BlendA8MaskRgb565(lv_draw_task_t* task,
                       const lv_draw_sw_blend_dsc_t* descriptor) {
    BlendA8Mask(task, descriptor, LV_COLOR_FORMAT_RGB565,
                PPA_BLEND_COLOR_MODE_RGB565, 2, s_previous_rgb565_handler);
}

void BlendA8MaskRgb888(lv_draw_task_t* task,
                       const lv_draw_sw_blend_dsc_t* descriptor) {
    BlendA8Mask(task, descriptor, LV_COLOR_FORMAT_RGB888,
                PPA_BLEND_COLOR_MODE_RGB888, 3, s_previous_rgb888_handler);
}

lv_draw_sw_custom_blend_handler_t s_a8_rgb565_handler = {
    .dest_cf = LV_COLOR_FORMAT_RGB565,
    .handler = BlendA8MaskRgb565,
};

lv_draw_sw_custom_blend_handler_t s_a8_rgb888_handler = {
    .dest_cf = LV_COLOR_FORMAT_RGB888,
    .handler = BlendA8MaskRgb888,
};

#endif

}  // namespace

void InitializeExpressionAcceleration() {
#if CONFIG_SOC_PPA_SUPPORTED
    if (s_registered) return;
    s_previous_rgb565_handler = lv_draw_sw_get_blend_handler(LV_COLOR_FORMAT_RGB565);
    s_previous_rgb888_handler = lv_draw_sw_get_blend_handler(LV_COLOR_FORMAT_RGB888);
    const ppa_client_config_t config = {.oper_type = PPA_OPERATION_BLEND};
    const esp_err_t result = ppa_register_client(&config, &s_blend_handle);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "PPA blend client registration failed: %s", esp_err_to_name(result));
        return;
    }

    if (!lv_draw_sw_register_blend_handler(&s_a8_rgb565_handler) ||
        !lv_draw_sw_register_blend_handler(&s_a8_rgb888_handler)) {
        ESP_LOGW(kTag, "RGB565/RGB888 A8 blend handler registration failed");
        ppa_unregister_client(s_blend_handle);
        s_blend_handle = nullptr;
        return;
    }
    s_registered = true;
    ESP_LOGI(kTag, "A8 blending uses PPA for RGB565 and RGB888 destinations");
#endif
}

void RegisterKeyboardRenderBuffer(const lv_draw_buf_t* buffer) {
#if CONFIG_SOC_PPA_SUPPORTED
    if (buffer == nullptr || buffer->data == nullptr || buffer->header.w == 0 ||
        buffer->header.h == 0 ||
        (buffer->header.cf != LV_COLOR_FORMAT_RGB565 && buffer->header.cf != LV_COLOR_FORMAT_RGB888)) return;
    InitializeExpressionAcceleration();
    if (!s_registered) return;
    s_keyboard_buffer = buffer;
    if (s_keyboard_copy_handle == nullptr) {
        const ppa_client_config_t config = {.oper_type = PPA_OPERATION_SRM};
        const esp_err_t result = ppa_register_client(&config, &s_keyboard_copy_handle);
        if (result != ESP_OK) {
            s_keyboard_copy_handle = nullptr;
            ESP_LOGW(kKeyboardTag, "copy client registration failed: %s", esp_err_to_name(result));
        }
    }
#else
    (void)buffer;
#endif
}

void UnregisterKeyboardRenderBuffer(const lv_draw_buf_t* buffer) {
#if CONFIG_SOC_PPA_SUPPORTED
    if (s_keyboard_buffer != buffer) return;
    s_keyboard_buffer = nullptr;
    if (s_keyboard_stats.hits != 0 || s_keyboard_stats.fallbacks != 0) LogKeyboardStats();
    s_keyboard_stats = {};
    if (s_keyboard_copy_handle != nullptr) {
        // SRM is blocking, so no copy can still reference the old cache here.
        const esp_err_t result = ppa_unregister_client(s_keyboard_copy_handle);
        if (result == ESP_OK) s_keyboard_copy_handle = nullptr;
        else ESP_LOGW(kKeyboardTag, "copy client release failed: %s", esp_err_to_name(result));
    }
#else
    (void)buffer;
#endif
}

void RegisterExpressionA8Buffer(const void* buffer, size_t size) {
#if CONFIG_SOC_PPA_SUPPORTED
    if (buffer == nullptr || size == 0) return;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(buffer);
    const uintptr_t end = begin + size;
    if (end <= begin) return;
    for (auto& range : s_expression_buffers) {
        if (range.begin == begin || range.begin == 0) {
            range = {.begin = begin, .end = end};
            return;
        }
    }
    ESP_LOGW(kTag, "Expression A8 registry full; buffer will use size threshold");
#else
    (void)buffer;
    (void)size;
#endif
}

void UnregisterExpressionA8Buffer(const void* buffer) {
#if CONFIG_SOC_PPA_SUPPORTED
    if (buffer == nullptr) return;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(buffer);
    for (auto& range : s_expression_buffers) {
        if (range.begin == begin) {
            range = {};
            return;
        }
    }
#else
    (void)buffer;
#endif
}

}  // namespace agent_ui
