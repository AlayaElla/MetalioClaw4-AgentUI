#include "pet_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace agent_ui::pet {
namespace {

constexpr char kTag[] = "PetRenderer";
// esp_lvgl_adapter's ESP32-P4 PPA path uses a 128-byte cache alignment.
// Matching it keeps the mutable ARGB foreground eligible for hardware blend.
constexpr size_t kBufferAlignment = 128;
constexpr float kTriangleEpsilon = 0.0001f;
constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

float Clamp(float value, float minimum, float maximum) {
    return std::max(minimum, std::min(maximum, value));
}

int ClampInt(int value, int minimum, int maximum) {
    return std::max(minimum, std::min(maximum, value));
}

Vec2 Add(Vec2 left, Vec2 right) {
    return {left.x + right.x, left.y + right.y};
}

Vec2 CubicBezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float time) {
    const float inverse = 1.0f - time;
    const float first = inverse * inverse * inverse;
    const float second = 3.0f * inverse * inverse * time;
    const float third = 3.0f * inverse * time * time;
    const float fourth = time * time * time;
    return {
        p0.x * first + p1.x * second + p2.x * third + p3.x * fourth,
        p0.y * first + p1.y * second + p2.y * third + p3.y * fourth,
    };
}

float TriangleArea(Vec2 first, Vec2 second, Vec2 third) {
    return (second.x - first.x) * (third.y - first.y) -
           (second.y - first.y) * (third.x - first.x);
}

lv_color32_t MakeColor(uint8_t red, uint8_t green, uint8_t blue,
                       uint8_t alpha) {
    return {
        .blue = blue,
        .green = green,
        .red = red,
        .alpha = alpha,
    };
}

lv_color32_t DecodeRgb565(uint16_t value, uint8_t alpha) {
    const uint8_t red = static_cast<uint8_t>(((value >> 11) & 0x1f) * 255 / 31);
    const uint8_t green =
        static_cast<uint8_t>(((value >> 5) & 0x3f) * 255 / 63);
    const uint8_t blue = static_cast<uint8_t>((value & 0x1f) * 255 / 31);
    return MakeColor(red, green, blue, alpha);
}

uint32_t MinimumTextureStride(uint8_t color_format, uint32_t width) {
    switch (color_format) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB565_SWAPPED:
        case LV_COLOR_FORMAT_RGB565A8:
            return width * 2U;
        case LV_COLOR_FORMAT_RGB888:
            return width * 3U;
        case LV_COLOR_FORMAT_ARGB8888:
        case LV_COLOR_FORMAT_XRGB8888:
            return width * 4U;
        default:
            return 0;
    }
}

uint32_t TextureStride(const lv_image_dsc_t& texture) {
    return texture.header.stride != 0
               ? texture.header.stride
               : MinimumTextureStride(texture.header.cf, texture.header.w);
}

}  // namespace

Renderer::Renderer(lv_obj_t* parent, uint16_t width, uint16_t height)
    : parent_(parent), width_(width), height_(height) {
    if (parent_ == nullptr || !lv_obj_is_valid(parent_) || width_ == 0 ||
        height_ == 0 || static_cast<uint32_t>(width_) *
                                sizeof(lv_color32_t) >
                            UINT16_MAX) {
        return;
    }

    const size_t buffer_size =
        static_cast<size_t>(width_) * height_ * sizeof(lv_color32_t);
    output_buffer_ = static_cast<lv_color32_t*>(heap_caps_aligned_alloc(
        kBufferAlignment, buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (output_buffer_ == nullptr) {
        output_buffer_ = static_cast<lv_color32_t*>(heap_caps_aligned_alloc(
            kBufferAlignment, buffer_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (output_buffer_ == nullptr) {
        ESP_LOGE(kTag, "Unable to allocate %u-byte ARGB8888 pet buffer",
                 static_cast<unsigned>(buffer_size));
        return;
    }
    std::memset(output_buffer_, 0, buffer_size);

    output_descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
    output_descriptor_.header.cf = LV_COLOR_FORMAT_ARGB8888;
    output_descriptor_.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
    output_descriptor_.header.w = width_;
    output_descriptor_.header.h = height_;
    output_descriptor_.header.stride = width_ * sizeof(lv_color32_t);
    output_descriptor_.data_size = buffer_size;
    output_descriptor_.data = reinterpret_cast<const uint8_t*>(output_buffer_);

    image_ = lv_image_create(parent_);
    lv_image_set_src(image_, &output_descriptor_);
    lv_image_set_inner_align(image_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(image_, width_, height_);
    lv_obj_center(image_);
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(parent_, ParentDeletedCallback, LV_EVENT_DELETE, this);
}

Renderer::~Renderer() {
    Stop();
}

bool Renderer::SetTexture(const lv_image_dsc_t* texture) {
    FreePreparedTexture();
    if (texture == nullptr || !ValidateTexture(*texture)) {
        texture_ = nullptr;
        ESP_LOGE(kTag,
                 "Pet texture must be uncompressed RGB565/RGB565A8/RGB888/ARGB8888/XRGB8888");
        return false;
    }
    texture_ = texture;
    RebuildPreparedTexture();
    return true;
}

bool Renderer::SetRig(const Rig& rig) {
    if (!ValidateRig(rig)) {
        has_rig_ = false;
        ESP_LOGE(kTag, "Invalid pet rig: use a 2..5 grid with <=25 vertices and <=4 limbs");
        return false;
    }
    rig_ = rig;
    has_rig_ = true;
    return true;
}

void Renderer::RebuildPreparedTexture() {
    if (texture_ == nullptr) return;
    const size_t pixel_count =
        static_cast<size_t>(texture_->header.w) * texture_->header.h;
    const size_t buffer_size = pixel_count * sizeof(lv_color32_t);
    auto* prepared = static_cast<lv_color32_t*>(heap_caps_aligned_alloc(
        kBufferAlignment, buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (prepared == nullptr) {
        ESP_LOGW(kTag,
                 "Unable to allocate %u-byte prepared texture; using source decode",
                 static_cast<unsigned>(buffer_size));
        return;
    }

    for (uint32_t y = 0; y < texture_->header.h; ++y) {
        const float v = texture_->header.h <= 1
                            ? 0.0f
                            : static_cast<float>(y) /
                                  (texture_->header.h - 1U);
        for (uint32_t x = 0; x < texture_->header.w; ++x) {
            const float u = texture_->header.w <= 1
                                ? 0.0f
                                : static_cast<float>(x) /
                                      (texture_->header.w - 1U);
            lv_color32_t sample{};
            if (!SampleTexture(u, v, &sample)) {
                heap_caps_free(prepared);
                ESP_LOGW(kTag, "Unable to prepare Pet texture");
                return;
            }
            prepared[static_cast<size_t>(y) * texture_->header.w + x] = sample;
        }
    }
    prepared_texture_ = prepared;
    prepared_texture_stride_pixels_ = texture_->header.w;
    ESP_LOGI(kTag, "Prepared %ux%u Pet texture (%u bytes)",
             static_cast<unsigned>(texture_->header.w),
             static_cast<unsigned>(texture_->header.h),
             static_cast<unsigned>(buffer_size));
}

void Renderer::FreePreparedTexture() {
    if (prepared_texture_ != nullptr) {
        heap_caps_free(prepared_texture_);
        prepared_texture_ = nullptr;
    }
    prepared_texture_stride_pixels_ = 0;
}

bool Renderer::Render(const Pose& pose) {
    if (!IsReady()) return false;
    const int64_t started_us = esp_timer_get_time();

    const int64_t raster_started_us = esp_timer_get_time();
    std::memset(output_buffer_, 0, output_descriptor_.data_size);
    DrawPose(pose);
    const uint32_t raster_us = static_cast<uint32_t>(std::max<int64_t>(
        0, esp_timer_get_time() - raster_started_us));
    lv_image_cache_drop(&output_descriptor_);
    lv_obj_invalidate(image_);
    RecordRenderTime(started_us, raster_us);
    return true;
}

void Renderer::DrawPose(const Pose& pose) {
    ComputeVertices(pose);
    DrawLimbs(pose, true);
    DrawBody();
    DrawLimbs(pose, false);
    if (debug_overlay_enabled_) DrawDebugOverlay();
}

void Renderer::RecordRenderTime(int64_t started_us, uint32_t raster_us,
                                uint32_t background_us, uint32_t overlay_us,
                                uint32_t submit_us) {
    const uint32_t elapsed_us = static_cast<uint32_t>(
        std::max<int64_t>(0, esp_timer_get_time() - started_us));
    ++stats_.frame_count;
    stats_.last_render_us = elapsed_us;
    stats_.last_raster_us = raster_us;
    stats_.last_background_us = background_us;
    stats_.last_overlay_us = overlay_us;
    stats_.last_submit_us = submit_us;
    stats_.max_render_us = std::max(stats_.max_render_us, elapsed_us);
    stats_.total_render_us += elapsed_us;
    if (elapsed_us > frame_period_ms_ * 1000U) {
        ++stats_.budget_overrun_count;
    }
}

bool Renderer::Play(const Clip* clip) {
    if (!IsReady() || !player_.Play(clip)) return false;
    Pose pose;
    if (player_.Sample(&pose)) Render(pose);
    last_tick_ms_ = lv_tick_get();
    if (frame_timer_ == nullptr) {
        frame_timer_ =
            lv_timer_create(FrameTimerCallback, frame_period_ms_, this);
    } else {
        lv_timer_set_period(frame_timer_, frame_period_ms_);
        lv_timer_resume(frame_timer_);
    }
    if (rendering_paused_) lv_timer_pause(frame_timer_);
    return true;
}

void Renderer::StopPlayback() {
    player_.Stop();
    if (frame_timer_ != nullptr) {
        lv_timer_delete(frame_timer_);
        frame_timer_ = nullptr;
    }
}

void Renderer::SetRenderingPaused(bool paused) {
    rendering_paused_ = paused;
    if (frame_timer_ == nullptr) return;
    if (paused) {
        lv_timer_pause(frame_timer_);
    } else {
        last_tick_ms_ = lv_tick_get();
        lv_timer_resume(frame_timer_);
    }
}

void Renderer::SetFramePeriodMs(uint32_t period_ms) {
    frame_period_ms_ = std::max<uint32_t>(16, period_ms);
    if (frame_timer_ != nullptr) {
        lv_timer_set_period(frame_timer_, frame_period_ms_);
    }
}

bool Renderer::IsReady() const {
    return parent_ != nullptr && image_ != nullptr && output_buffer_ != nullptr &&
           lv_obj_is_valid(image_) && texture_ != nullptr && has_rig_;
}

bool Renderer::ValidateTexture(const lv_image_dsc_t& texture) const {
    if (texture.data == nullptr || texture.header.magic != LV_IMAGE_HEADER_MAGIC ||
        texture.header.w == 0 || texture.header.h == 0 ||
        (texture.header.flags & LV_IMAGE_FLAGS_COMPRESSED) != 0 ||
        (texture.header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED) != 0) {
        return false;
    }

    const uint32_t minimum_stride =
        MinimumTextureStride(texture.header.cf, texture.header.w);
    if (minimum_stride == 0) return false;
    const uint32_t stride = TextureStride(texture);
    if (stride < minimum_stride) return false;
    size_t required = static_cast<size_t>(stride) * texture.header.h;
    if (texture.header.cf == LV_COLOR_FORMAT_RGB565A8) {
        required += static_cast<size_t>(stride / 2U) * texture.header.h;
    }
    return texture.data_size >= required;
}

bool Renderer::ValidateRig(const Rig& rig) const {
    const uint32_t vertex_count =
        static_cast<uint32_t>(rig.body.columns) * rig.body.rows;
    if (rig.body.columns < 2 || rig.body.rows < 2 || vertex_count == 0 ||
        rig.body.columns > kMaxGridAxis || rig.body.rows > kMaxGridAxis ||
        vertex_count > kMaxBodyVertices || rig.body.destination.width <= 0.0f ||
        rig.body.destination.height <= 0.0f || rig.limb_count > kMaxLimbs) {
        return false;
    }
    for (size_t index = 0; index < rig.limb_count; ++index) {
        if (rig.limbs[index].anchor_vertex >= vertex_count ||
            rig.limbs[index].width <= 0.0f) {
            return false;
        }
    }
    return true;
}

void Renderer::ComputeVertices(const Pose& pose) {
    const size_t columns = rig_.body.columns;
    const size_t rows = rig_.body.rows;
    for (size_t row = 0; row < rows; ++row) {
        const float vertical =
            rows == 1 ? 0.0f : static_cast<float>(row) / (rows - 1);
        for (size_t column = 0; column < columns; ++column) {
            const float horizontal = columns == 1
                                         ? 0.0f
                                         : static_cast<float>(column) /
                                               (columns - 1);
            const size_t index = row * columns + column;
            Vec2 point = {
                rig_.body.destination.x +
                    rig_.body.destination.width * horizontal,
                rig_.body.destination.y +
                    rig_.body.destination.height * vertical,
            };
            point = Add(point, pose.body_vertex_offsets[index]);
            vertices_[index] = TransformPoint(point, pose);
            texture_coordinates_[index] = {
                rig_.body.uv.x + rig_.body.uv.width * horizontal,
                rig_.body.uv.y + rig_.body.uv.height * vertical,
            };
        }
    }
}

void Renderer::DrawBody() {
    const size_t columns = rig_.body.columns;
    const size_t rows = rig_.body.rows;
    for (size_t row = 0; row + 1 < rows; ++row) {
        for (size_t column = 0; column + 1 < columns; ++column) {
            const size_t top_left = row * columns + column;
            const size_t top_right = top_left + 1;
            const size_t bottom_left = top_left + columns;
            const size_t bottom_right = bottom_left + 1;
            DrawTriangle(top_left, bottom_left, top_right);
            DrawTriangle(top_right, bottom_left, bottom_right);
        }
    }
}

void Renderer::DrawTriangle(size_t first, size_t second, size_t third) {
    const Vec2 a = vertices_[first];
    const Vec2 b = vertices_[second];
    const Vec2 c = vertices_[third];
    const float area = TriangleArea(a, b, c);
    if (std::abs(area) <= kTriangleEpsilon) return;

    const int min_x = ClampInt(
        static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))), 0,
        static_cast<int>(width_) - 1);
    const int max_x = ClampInt(
        static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))), 0,
        static_cast<int>(width_) - 1);
    const int min_y = ClampInt(
        static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))), 0,
        static_cast<int>(height_) - 1);
    const int max_y = ClampInt(
        static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))), 0,
        static_cast<int>(height_) - 1);
    if (min_x > max_x || min_y > max_y) return;

    const Vec2 uv_a = texture_coordinates_[first];
    const Vec2 uv_b = texture_coordinates_[second];
    const Vec2 uv_c = texture_coordinates_[third];

    // Barycentric weights and UVs are affine across a triangle. Compute their
    // derivatives once, then advance them with additions in the pixel loop.
    // This removes two cross products and two divisions from every covered
    // pixel, which is the dominant CPU cost for a deforming textured body.
    const float inverse_area = 1.0f / area;
    const float weight_a_dx = (b.y - c.y) * inverse_area;
    const float weight_a_dy = (c.x - b.x) * inverse_area;
    const float weight_b_dx = (c.y - a.y) * inverse_area;
    const float weight_b_dy = (a.x - c.x) * inverse_area;
    const float weight_c_dx = -weight_a_dx - weight_b_dx;
    const float weight_c_dy = -weight_a_dy - weight_b_dy;

    const float u_dx = uv_a.x * weight_a_dx + uv_b.x * weight_b_dx +
                       uv_c.x * weight_c_dx;
    const float u_dy = uv_a.x * weight_a_dy + uv_b.x * weight_b_dy +
                       uv_c.x * weight_c_dy;
    const float v_dx = uv_a.y * weight_a_dx + uv_b.y * weight_b_dx +
                       uv_c.y * weight_c_dx;
    const float v_dy = uv_a.y * weight_a_dy + uv_b.y * weight_b_dy +
                       uv_c.y * weight_c_dy;

    const Vec2 first_pixel = {min_x + 0.5f, min_y + 0.5f};
    float weight_a_row = TriangleArea(first_pixel, b, c) * inverse_area;
    float weight_b_row = TriangleArea(first_pixel, c, a) * inverse_area;
    float weight_c_row = 1.0f - weight_a_row - weight_b_row;
    float u_row = uv_a.x * weight_a_row + uv_b.x * weight_b_row +
                  uv_c.x * weight_c_row;
    float v_row = uv_a.y * weight_a_row + uv_b.y * weight_b_row +
                  uv_c.y * weight_c_row;
    if (prepared_texture_ != nullptr) {
        const float texture_width = texture_->header.w - 1U;
        const float texture_height = texture_->header.h - 1U;
        const float texture_x_dx = u_dx * texture_width;
        const float texture_x_dy = u_dy * texture_width;
        const float texture_y_dx = v_dx * texture_height;
        const float texture_y_dy = v_dy * texture_height;
        float texture_x_row = u_row * texture_width;
        float texture_y_row = v_row * texture_height;
        for (int y = min_y; y <= max_y; ++y) {
            float weight_a = weight_a_row;
            float weight_b = weight_b_row;
            float weight_c = weight_c_row;
            float texture_x = texture_x_row;
            float texture_y = texture_y_row;
            for (int x = min_x; x <= max_x; ++x) {
                if (weight_a >= -kTriangleEpsilon &&
                    weight_b >= -kTriangleEpsilon &&
                    weight_c >= -kTriangleEpsilon) {
                    const int source_x = ClampInt(
                        static_cast<int>(texture_x + 0.5f), 0,
                        texture_->header.w - 1);
                    const int source_y = ClampInt(
                        static_cast<int>(texture_y + 0.5f), 0,
                        texture_->header.h - 1);
                    BlendPixel(
                        x, y,
                        prepared_texture_[static_cast<size_t>(source_y) *
                                              prepared_texture_stride_pixels_ +
                                          source_x]);
                }
                weight_a += weight_a_dx;
                weight_b += weight_b_dx;
                weight_c += weight_c_dx;
                texture_x += texture_x_dx;
                texture_y += texture_y_dx;
            }
            weight_a_row += weight_a_dy;
            weight_b_row += weight_b_dy;
            weight_c_row += weight_c_dy;
            texture_x_row += texture_x_dy;
            texture_y_row += texture_y_dy;
        }
        return;
    }
    for (int y = min_y; y <= max_y; ++y) {
        float weight_a = weight_a_row;
        float weight_b = weight_b_row;
        float weight_c = weight_c_row;
        float u = u_row;
        float v = v_row;
        for (int x = min_x; x <= max_x; ++x) {
            if (weight_a >= -kTriangleEpsilon &&
                weight_b >= -kTriangleEpsilon &&
                weight_c >= -kTriangleEpsilon) {
                lv_color32_t sample;
                if (SampleTexture(u, v, &sample)) BlendPixel(x, y, sample);
            }
            weight_a += weight_a_dx;
            weight_b += weight_b_dx;
            weight_c += weight_c_dx;
            u += u_dx;
            v += v_dx;
        }
        weight_a_row += weight_a_dy;
        weight_b_row += weight_b_dy;
        weight_c_row += weight_c_dy;
        u_row += u_dy;
        v_row += v_dy;
    }
}

void Renderer::DrawLimbs(const Pose& pose, bool behind_body) {
    for (size_t index = 0; index < rig_.limb_count; ++index) {
        if (rig_.limbs[index].draw_behind_body == behind_body) {
            DrawLimb(rig_.limbs[index], pose.limbs[index], pose);
        }
    }
}

void Renderer::DrawLimb(const LimbRig& rig, const LimbPose& pose,
                        const Pose& frame_pose) {
    const Vec2 start = vertices_[rig.anchor_vertex];
    const Vec2 control1 = Add(
        start,
        TransformVector(Add(rig.control1, pose.control1_offset), frame_pose));
    const Vec2 control2 = Add(
        start,
        TransformVector(Add(rig.control2, pose.control2_offset), frame_pose));
    const Vec2 end = Add(
        start, TransformVector(Add(rig.end, pose.end_offset), frame_pose));

    const int segments = ClampInt(rig.segments, 1, 16);
    Vec2 previous = start;
    for (int segment = 1; segment <= segments; ++segment) {
        const float time = static_cast<float>(segment) / segments;
        const Vec2 next = CubicBezier(start, control1, control2, end, time);
        DrawSegment(previous, next, rig.width * std::max(0.0f, pose.width_scale),
                    rig.color, Clamp(pose.opacity, 0.0f, 1.0f));
        previous = next;
    }
}

void Renderer::DrawDebugOverlay() {
    constexpr RgbaColor kGridColor{24, 173, 229, 190};
    constexpr RgbaColor kAnchorColor{255, 72, 142, 255};
    const size_t columns = rig_.body.columns;
    const size_t rows = rig_.body.rows;
    for (size_t row = 0; row < rows; ++row) {
        for (size_t column = 0; column + 1 < columns; ++column) {
            const size_t first = row * columns + column;
            DrawSegment(vertices_[first], vertices_[first + 1], 1.5f,
                        kGridColor, 1.0f);
        }
    }
    for (size_t column = 0; column < columns; ++column) {
        for (size_t row = 0; row + 1 < rows; ++row) {
            const size_t first = row * columns + column;
            DrawSegment(vertices_[first], vertices_[first + columns], 1.5f,
                        kGridColor, 1.0f);
        }
    }
    for (size_t index = 0; index < rig_.limb_count; ++index) {
        const Vec2 anchor = vertices_[rig_.limbs[index].anchor_vertex];
        DrawSegment(anchor, anchor, 7.0f, kAnchorColor, 1.0f);
    }
}

void Renderer::DrawSegment(Vec2 from, Vec2 to, float width,
                           const RgbaColor& color, float opacity) {
    if (width <= 0.0f || opacity <= 0.0f) return;
    const float radius = width * 0.5f;
    const int min_x = ClampInt(
        static_cast<int>(std::floor(std::min(from.x, to.x) - radius - 1.0f)),
        0, static_cast<int>(width_) - 1);
    const int max_x = ClampInt(
        static_cast<int>(std::ceil(std::max(from.x, to.x) + radius + 1.0f)),
        0, static_cast<int>(width_) - 1);
    const int min_y = ClampInt(
        static_cast<int>(std::floor(std::min(from.y, to.y) - radius - 1.0f)),
        0, static_cast<int>(height_) - 1);
    const int max_y = ClampInt(
        static_cast<int>(std::ceil(std::max(from.y, to.y) + radius + 1.0f)),
        0, static_cast<int>(height_) - 1);
    const float delta_x = to.x - from.x;
    const float delta_y = to.y - from.y;
    const float length_squared = delta_x * delta_x + delta_y * delta_y;
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const float pixel_x = x + 0.5f;
            const float pixel_y = y + 0.5f;
            const float amount =
                length_squared <= kTriangleEpsilon
                    ? 0.0f
                    : Clamp(((pixel_x - from.x) * delta_x +
                             (pixel_y - from.y) * delta_y) /
                                length_squared,
                            0.0f, 1.0f);
            const float nearest_x = from.x + delta_x * amount;
            const float nearest_y = from.y + delta_y * amount;
            const float distance_x = pixel_x - nearest_x;
            const float distance_y = pixel_y - nearest_y;
            const float distance =
                std::sqrt(distance_x * distance_x + distance_y * distance_y);
            const float coverage = Clamp(radius + 0.5f - distance, 0.0f, 1.0f);
            if (coverage <= 0.0f) continue;
            const uint8_t alpha = static_cast<uint8_t>(
                color.alpha * opacity * coverage + 0.5f);
            BlendPixel(x, y,
                       MakeColor(color.red, color.green, color.blue, alpha));
        }
    }
}

bool Renderer::SampleTexture(float u, float v, lv_color32_t* output) const {
    if (texture_ == nullptr || output == nullptr) return false;
    const int x = ClampInt(
        static_cast<int>(Clamp(u, 0.0f, 1.0f) *
                             (texture_->header.w - 1) +
                         0.5f),
        0, texture_->header.w - 1);
    const int y = ClampInt(
        static_cast<int>(Clamp(v, 0.0f, 1.0f) *
                             (texture_->header.h - 1) +
                         0.5f),
        0, texture_->header.h - 1);
    const uint32_t stride = TextureStride(*texture_);
    const uint8_t* row = texture_->data + static_cast<size_t>(stride) * y;
    switch (texture_->header.cf) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB565_SWAPPED: {
            uint16_t value;
            std::memcpy(&value, row + x * 2, sizeof(value));
            if (texture_->header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED) {
                value = static_cast<uint16_t>((value >> 8) | (value << 8));
            }
            *output = DecodeRgb565(value, 255);
            return true;
        }
        case LV_COLOR_FORMAT_RGB565A8: {
            uint16_t value;
            std::memcpy(&value, row + x * 2, sizeof(value));
            const uint32_t alpha_stride = stride / 2U;
            const uint8_t* alpha = texture_->data +
                                   static_cast<size_t>(stride) *
                                       texture_->header.h +
                                   static_cast<size_t>(alpha_stride) * y;
            *output = DecodeRgb565(value, alpha[x]);
            return true;
        }
        case LV_COLOR_FORMAT_RGB888:
            *output = MakeColor(row[x * 3 + 2], row[x * 3 + 1],
                                row[x * 3], 255);
            return true;
        case LV_COLOR_FORMAT_ARGB8888: {
            lv_color32_t color;
            std::memcpy(&color, row + x * 4, sizeof(color));
            *output = color;
            return true;
        }
        case LV_COLOR_FORMAT_XRGB8888:
            *output = MakeColor(row[x * 4 + 2], row[x * 4 + 1],
                                row[x * 4], 255);
            return true;
        default:
            return false;
    }
}

void Renderer::BlendPixel(int x, int y, lv_color32_t source) {
    if (source.alpha == 0 || x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }
    lv_color32_t& destination =
        output_buffer_[static_cast<size_t>(y) * width_ + x];
    if (source.alpha == 255 || destination.alpha == 0) {
        destination = source;
        return;
    }

    const uint32_t source_alpha = source.alpha;
    const uint32_t destination_alpha = destination.alpha;
    const uint32_t inverse = 255U - source_alpha;
    const uint32_t output_alpha =
        source_alpha + (destination_alpha * inverse + 127U) / 255U;
    if (output_alpha == 0) {
        destination = {};
        return;
    }
    const auto blend_channel = [&](uint8_t source_channel,
                                   uint8_t destination_channel) {
        const uint32_t numerator =
            static_cast<uint32_t>(source_channel) * source_alpha * 255U +
            static_cast<uint32_t>(destination_channel) * destination_alpha *
                inverse;
        return static_cast<uint8_t>(
            (numerator + output_alpha * 127U) / (output_alpha * 255U));
    };
    destination.red = blend_channel(source.red, destination.red);
    destination.green = blend_channel(source.green, destination.green);
    destination.blue = blend_channel(source.blue, destination.blue);
    destination.alpha = static_cast<uint8_t>(output_alpha);
}

Vec2 Renderer::TransformPoint(Vec2 point, const Pose& pose) const {
    const Vec2 relative = {
        (point.x - rig_.root_pivot.x) * pose.root_scale.x,
        (point.y - rig_.root_pivot.y) * pose.root_scale.y,
    };
    const float radians = pose.root_rotation_degrees * kDegreesToRadians;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    return {
        rig_.root_pivot.x + relative.x * cosine - relative.y * sine +
            pose.root_translation.x,
        rig_.root_pivot.y + relative.x * sine + relative.y * cosine +
            pose.root_translation.y,
    };
}

Vec2 Renderer::TransformVector(Vec2 vector, const Pose& pose) const {
    const Vec2 scaled = {
        vector.x * pose.root_scale.x,
        vector.y * pose.root_scale.y,
    };
    const float radians = pose.root_rotation_degrees * kDegreesToRadians;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    return {
        scaled.x * cosine - scaled.y * sine,
        scaled.x * sine + scaled.y * cosine,
    };
}

void Renderer::FrameTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<Renderer*>(lv_timer_get_user_data(timer));
    if (self == nullptr || self->rendering_paused_) return;
    const uint32_t now = lv_tick_get();
    const uint32_t delta = lv_tick_elaps(self->last_tick_ms_);
    self->last_tick_ms_ = now;
    if (self->frame_period_ms_ > 0) {
        const uint32_t elapsed_periods = delta / self->frame_period_ms_;
        if (elapsed_periods > 1) {
            self->stats_.missed_frame_count += elapsed_periods - 1;
        }
    }
    self->player_.Advance(delta);
    Pose pose;
    if (self->player_.Sample(&pose)) self->Render(pose);
    if (!self->player_.IsPlaying()) lv_timer_pause(timer);
}

void Renderer::ParentDeletedCallback(lv_event_t* event) {
    auto* self = static_cast<Renderer*>(lv_event_get_user_data(event));
    if (self == nullptr) return;
    self->StopPlayback();
    self->parent_ = nullptr;
    self->image_ = nullptr;
}

void Renderer::Stop() {
    StopPlayback();
    FreePreparedTexture();
    if (parent_ != nullptr && lv_obj_is_valid(parent_)) {
        lv_obj_remove_event_cb_with_user_data(parent_, ParentDeletedCallback,
                                              this);
    }
    if (image_ != nullptr && lv_obj_is_valid(image_)) lv_obj_delete(image_);
    image_ = nullptr;
    parent_ = nullptr;
    if (output_buffer_ != nullptr) {
        lv_image_cache_drop(&output_descriptor_);
        heap_caps_free(output_buffer_);
        output_buffer_ = nullptr;
        output_descriptor_.data = nullptr;
    }
}

}  // namespace agent_ui::pet
