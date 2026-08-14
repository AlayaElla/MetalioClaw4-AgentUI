#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace agent_ui::pet {

constexpr uint32_t kAnimationDataVersion = 1;
constexpr size_t kMaxGridAxis = 5;
constexpr size_t kMaxBodyVertices = kMaxGridAxis * kMaxGridAxis;
constexpr size_t kMaxLimbs = 4;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// Cubic easing from (0, 0) to (1, 1). The input time is used directly as
// the Bezier parameter so y1/y2 may deliberately overshoot for squash/bounce.
struct Curve {
    float y1 = 0.0f;
    float y2 = 1.0f;

    float Evaluate(float time) const;

    static constexpr Curve Linear() { return {1.0f / 3.0f, 2.0f / 3.0f}; }
    static constexpr Curve EaseInOut() { return {0.0f, 1.0f}; }
};

// The three offsets are relative to a limb's body vertex anchor and are added
// to the rest cubic Bezier points stored in LimbRig.
struct LimbPose {
    Vec2 control1_offset{};
    Vec2 control2_offset{};
    Vec2 end_offset{};
    float width_scale = 1.0f;
    float opacity = 1.0f;
};

// Fixed-capacity frame data keeps playback deterministic and allocation-free.
// Unused body/limb entries remain zero/neutral.
struct Pose {
    Vec2 root_translation{};
    float root_rotation_degrees = 0.0f;
    Vec2 root_scale{1.0f, 1.0f};
    std::array<Vec2, kMaxBodyVertices> body_vertex_offsets{};
    std::array<LimbPose, kMaxLimbs> limbs{};
};

struct Keyframe {
    uint32_t time_ms = 0;
    Pose pose{};
    Curve curve_to_next = Curve::EaseInOut();
};

// Keyframes and the clip itself may live in flash. Their lifetime must exceed
// playback. duration_ms == 0 uses the last keyframe time.
struct Clip {
    const char* id = nullptr;
    const Keyframe* keyframes = nullptr;
    size_t keyframe_count = 0;
    uint32_t duration_ms = 0;
    bool loop = true;
};

class AnimationPlayer {
public:
    bool Play(const Clip* clip);
    void Stop();
    void Advance(uint32_t delta_ms);
    void Seek(uint32_t time_ms);
    bool Sample(Pose* output) const;

    bool IsPlaying() const { return playing_; }
    uint32_t ElapsedMs() const { return elapsed_ms_; }
    const Clip* CurrentClip() const { return clip_; }

private:
    static bool ValidateClip(const Clip& clip);
    static uint32_t ResolveDuration(const Clip& clip);
    static Pose Interpolate(const Pose& from, const Pose& to, float amount);

    const Clip* clip_ = nullptr;
    uint32_t elapsed_ms_ = 0;
    bool playing_ = false;
};

}  // namespace agent_ui::pet
