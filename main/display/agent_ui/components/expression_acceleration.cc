#include "expression_acceleration.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
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
