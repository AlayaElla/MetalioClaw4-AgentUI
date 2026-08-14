#include "pet_animation.h"

#include <algorithm>

namespace agent_ui::pet {
namespace {

float Clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

float Lerp(float from, float to, float amount) {
    return from + (to - from) * amount;
}

Vec2 Lerp(Vec2 from, Vec2 to, float amount) {
    return {Lerp(from.x, to.x, amount), Lerp(from.y, to.y, amount)};
}

LimbPose Lerp(const LimbPose& from, const LimbPose& to, float amount) {
    LimbPose result;
    result.control1_offset =
        Lerp(from.control1_offset, to.control1_offset, amount);
    result.control2_offset =
        Lerp(from.control2_offset, to.control2_offset, amount);
    result.end_offset = Lerp(from.end_offset, to.end_offset, amount);
    result.width_scale = Lerp(from.width_scale, to.width_scale, amount);
    result.opacity = Lerp(from.opacity, to.opacity, amount);
    return result;
}

}  // namespace

float Curve::Evaluate(float time) const {
    const float t = Clamp01(time);
    const float inverse = 1.0f - t;
    return 3.0f * inverse * inverse * t * y1 +
           3.0f * inverse * t * t * y2 + t * t * t;
}

bool AnimationPlayer::Play(const Clip* clip) {
    if (clip == nullptr || !ValidateClip(*clip)) {
        Stop();
        return false;
    }
    clip_ = clip;
    elapsed_ms_ = 0;
    playing_ = true;
    return true;
}

void AnimationPlayer::Stop() {
    clip_ = nullptr;
    elapsed_ms_ = 0;
    playing_ = false;
}

void AnimationPlayer::Advance(uint32_t delta_ms) {
    if (!playing_ || clip_ == nullptr) return;
    const uint32_t duration = ResolveDuration(*clip_);
    if (duration == 0) {
        playing_ = false;
        return;
    }

    const uint64_t next = static_cast<uint64_t>(elapsed_ms_) + delta_ms;
    if (clip_->loop) {
        elapsed_ms_ = static_cast<uint32_t>(next % duration);
    } else if (next >= duration) {
        elapsed_ms_ = duration;
        playing_ = false;
    } else {
        elapsed_ms_ = static_cast<uint32_t>(next);
    }
}

void AnimationPlayer::Seek(uint32_t time_ms) {
    if (clip_ == nullptr) return;
    const uint32_t duration = ResolveDuration(*clip_);
    if (duration == 0) {
        elapsed_ms_ = 0;
    } else if (clip_->loop) {
        elapsed_ms_ = time_ms % duration;
    } else {
        elapsed_ms_ = std::min(time_ms, duration);
    }
}

bool AnimationPlayer::Sample(Pose* output) const {
    if (output == nullptr || clip_ == nullptr || clip_->keyframes == nullptr ||
        clip_->keyframe_count == 0) {
        return false;
    }

    const Keyframe* frames = clip_->keyframes;
    if (clip_->keyframe_count == 1 || elapsed_ms_ <= frames[0].time_ms) {
        *output = frames[0].pose;
        return true;
    }

    for (size_t index = 1; index < clip_->keyframe_count; ++index) {
        const Keyframe& next = frames[index];
        const Keyframe& previous = frames[index - 1];
        if (elapsed_ms_ <= next.time_ms) {
            const uint32_t span = next.time_ms - previous.time_ms;
            const float segment_time =
                span == 0
                    ? 1.0f
                    : static_cast<float>(elapsed_ms_ - previous.time_ms) /
                          static_cast<float>(span);
            const float amount = previous.curve_to_next.Evaluate(segment_time);
            *output = Interpolate(previous.pose, next.pose, amount);
            return true;
        }
    }

    *output = frames[clip_->keyframe_count - 1].pose;
    return true;
}

uint32_t AnimationPlayer::ResolveDuration(const Clip& clip) {
    if (clip.duration_ms != 0) return clip.duration_ms;
    if (clip.keyframes == nullptr || clip.keyframe_count == 0) return 0;
    return clip.keyframes[clip.keyframe_count - 1].time_ms;
}

bool AnimationPlayer::ValidateClip(const Clip& clip) {
    if (clip.keyframes == nullptr || clip.keyframe_count == 0) return false;
    const uint32_t duration = ResolveDuration(clip);
    if (duration == 0 || clip.keyframes[clip.keyframe_count - 1].time_ms > duration) {
        return false;
    }
    for (size_t index = 1; index < clip.keyframe_count; ++index) {
        if (clip.keyframes[index].time_ms <
            clip.keyframes[index - 1].time_ms) {
            return false;
        }
    }
    return true;
}

Pose AnimationPlayer::Interpolate(const Pose& from, const Pose& to,
                                  float amount) {
    Pose result;
    result.root_translation =
        Lerp(from.root_translation, to.root_translation, amount);
    result.root_rotation_degrees =
        Lerp(from.root_rotation_degrees, to.root_rotation_degrees, amount);
    result.root_scale = Lerp(from.root_scale, to.root_scale, amount);
    for (size_t index = 0; index < result.body_vertex_offsets.size(); ++index) {
        result.body_vertex_offsets[index] =
            Lerp(from.body_vertex_offsets[index],
                 to.body_vertex_offsets[index], amount);
    }
    for (size_t index = 0; index < result.limbs.size(); ++index) {
        result.limbs[index] = Lerp(from.limbs[index], to.limbs[index], amount);
    }
    return result;
}

}  // namespace agent_ui::pet
