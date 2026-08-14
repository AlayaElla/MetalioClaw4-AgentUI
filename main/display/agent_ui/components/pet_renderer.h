#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "lvgl.h"
#include "pet_animation.h"

namespace agent_ui::pet {

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct RgbaColor {
    uint8_t red = 255;
    uint8_t green = 255;
    uint8_t blue = 255;
    uint8_t alpha = 255;
};

struct BodyGrid {
    uint8_t columns = 5;
    uint8_t rows = 5;
    Rect destination{};
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
};

// Cubic Bezier rest geometry is relative to the body anchor vertex.
struct LimbRig {
    uint8_t anchor_vertex = 0;
    Vec2 control1{};
    Vec2 control2{};
    Vec2 end{};
    float width = 4.0f;
    RgbaColor color{};
    uint8_t segments = 8;
    bool draw_behind_body = false;
};

struct Rig {
    BodyGrid body{};
    Vec2 root_pivot{};
    std::array<LimbRig, kMaxLimbs> limbs{};
    uint8_t limb_count = 0;
};

struct RenderStats {
    uint32_t frame_count = 0;
    uint32_t last_render_us = 0;
    uint32_t last_raster_us = 0;
    uint32_t last_background_us = 0;
    uint32_t last_overlay_us = 0;
    uint32_t last_submit_us = 0;
    uint32_t max_render_us = 0;
    uint64_t total_render_us = 0;
    uint32_t budget_overrun_count = 0;
    uint32_t missed_frame_count = 0;

    uint32_t AverageRenderUs() const {
        return frame_count == 0
                   ? 0
                   : static_cast<uint32_t>(total_render_us / frame_count);
    }
};

// LVGL-thread-only textured mesh and Bezier limb renderer. The texture and
// animation clips remain caller-owned; the rig is copied into the renderer.
class Renderer {
public:
    Renderer(lv_obj_t* parent, uint16_t width, uint16_t height);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool SetTexture(const lv_image_dsc_t* texture);
    bool SetRig(const Rig& rig);
    bool Render(const Pose& pose);

    bool Play(const Clip* clip);
    void StopPlayback();
    void SetRenderingPaused(bool paused);
    void SetFramePeriodMs(uint32_t period_ms);
    void SetDebugOverlayEnabled(bool enabled) { debug_overlay_enabled_ = enabled; }
    bool IsReady() const;
    lv_obj_t* Object() const { return image_; }
    const AnimationPlayer& Player() const { return player_; }
    const RenderStats& Stats() const { return stats_; }
    void ResetStats() { stats_ = {}; }
    size_t BufferBytes() const {
        return static_cast<size_t>(width_) * height_ * sizeof(lv_color32_t);
    }

private:
    static void FrameTimerCallback(lv_timer_t* timer);
    static void ParentDeletedCallback(lv_event_t* event);

    bool ValidateTexture(const lv_image_dsc_t& texture) const;
    bool ValidateRig(const Rig& rig) const;
    void ComputeVertices(const Pose& pose);
    void DrawBody();
    void DrawTriangle(size_t first, size_t second, size_t third);
    void DrawLimbs(const Pose& pose, bool behind_body);
    void DrawLimb(const LimbRig& rig, const LimbPose& pose,
                  const Pose& frame_pose);
    void DrawDebugOverlay();
    void DrawSegment(Vec2 from, Vec2 to, float width,
                     const RgbaColor& color, float opacity);
    bool SampleTexture(float u, float v, lv_color32_t* output) const;
    void BlendPixel(int x, int y, lv_color32_t source);
    void DrawPose(const Pose& pose);
    void RecordRenderTime(int64_t started_us, uint32_t raster_us,
                          uint32_t background_us = 0,
                          uint32_t overlay_us = 0,
                          uint32_t submit_us = 0);
    void RebuildPreparedTexture();
    void FreePreparedTexture();
    Vec2 TransformPoint(Vec2 point, const Pose& pose) const;
    Vec2 TransformVector(Vec2 vector, const Pose& pose) const;
    void Stop();

    lv_obj_t* parent_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_timer_t* frame_timer_ = nullptr;
    lv_color32_t* output_buffer_ = nullptr;
    lv_image_dsc_t output_descriptor_{};
    const lv_image_dsc_t* texture_ = nullptr;
    lv_color32_t* prepared_texture_ = nullptr;
    uint32_t prepared_texture_stride_pixels_ = 0;
    Rig rig_{};
    bool has_rig_ = false;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint32_t frame_period_ms_ = 33;
    uint32_t last_tick_ms_ = 0;
    bool rendering_paused_ = false;
    bool debug_overlay_enabled_ = false;
    AnimationPlayer player_{};
    RenderStats stats_{};
    std::array<Vec2, kMaxBodyVertices> vertices_{};
    std::array<Vec2, kMaxBodyVertices> texture_coordinates_{};
};

}  // namespace agent_ui::pet
