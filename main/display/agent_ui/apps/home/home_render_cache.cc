#include "home_render_cache.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace agent_ui::home {

CarouselRenderCache::~CarouselRenderCache() { Release(); }

bool CarouselRenderCache::Capture(lv_obj_t* source, lv_obj_t* image) {
#if !LV_USE_SNAPSHOT
    (void)source;
    (void)image;
    return false;
#else
    const auto fail_capture = [this]() {
        Release();
        return false;
    };
    if (source == nullptr || image == nullptr || !lv_obj_is_valid(source) ||
        !lv_obj_is_valid(image)) {
        return fail_capture();
    }

    lv_obj_update_layout(source);
    // The Home carousel removes all styles before sizing itself, so it has no
    // shadow/outline extent beyond its box. Snapshot only that packed box.
    const int32_t width = lv_obj_get_width(source);
    const int32_t height = lv_obj_get_height(source);
    if (width <= 0 || height <= 0 ||
        static_cast<uint64_t>(width) * 3U >
            std::numeric_limits<uint32_t>::max()) {
        return fail_capture();
    }

    constexpr lv_color_format_t kFormat = LV_COLOR_FORMAT_RGB888;
    const uint32_t stride = lv_draw_buf_width_to_stride(
        static_cast<uint32_t>(width), kFormat);
    // The opaque-copy PPA path accepts tightly packed RGB888 rows only.
    if (stride != static_cast<uint32_t>(width) * 3U ||
        static_cast<size_t>(stride) >
            std::numeric_limits<size_t>::max() /
                static_cast<size_t>(height)) {
        return fail_capture();
    }
    RenderSnapshotBuffer candidate;
    if (!candidate.Prepare(static_cast<uint32_t>(width),
                           static_cast<uint32_t>(height), kFormat) ||
        !candidate.Capture(source)) {
        return fail_capture();
    }
    const lv_draw_buf_t* candidate_buffer = candidate.buffer();
    if (candidate_buffer == nullptr ||
        candidate_buffer->header.cf != kFormat ||
        candidate_buffer->header.w != width ||
        candidate_buffer->header.h != height ||
        candidate_buffer->header.stride != stride) {
        return fail_capture();
    }

    // Detach the image before unregistering or releasing its old pixels.
    if (image_ != nullptr && image_ != image && lv_obj_is_valid(image_)) {
        lv_image_set_src(image_, nullptr);
    }
    lv_image_set_src(image, nullptr);
    snapshot_.Adopt(candidate);
    image_ = image;
    snapshot_.RegisterOpaque("HomeCarouselPPA");
    lv_image_set_src(image_, snapshot_.buffer());
    return true;
#endif
}

void CarouselRenderCache::Release() {
    if (image_ != nullptr && lv_obj_is_valid(image_)) {
        lv_image_set_src(image_, nullptr);
    }
    snapshot_.Release();
    image_ = nullptr;
}

}  // namespace agent_ui::home
