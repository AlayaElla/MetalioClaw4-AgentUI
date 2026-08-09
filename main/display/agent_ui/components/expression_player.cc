#include "expression_player.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "expression_acceleration.h"
#include "core/performance_manager.h"
#include "core/theme.h"

namespace agent_ui {
namespace {

constexpr char kTag[] = "ExpressionPlayer";
constexpr uint32_t kIdleDelayMinMs = 6000;
constexpr uint32_t kIdleDelayRangeMs = 8001;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = kPi * 2.0f;
constexpr float kGridScale = 56.0f / 48.0f;
constexpr float kPivot = 24.0f * kGridScale;
constexpr uint32_t kListeningAudioFrameMs = 60;
constexpr uint32_t kListeningActivityAttackMs = 90;
constexpr uint32_t kListeningActivityReleaseMs = 420;
constexpr uint32_t kListeningVadAttackMs = 55;
constexpr uint32_t kListeningVadReleaseMs = 260;
constexpr uint32_t kListeningVadHoldMs = 180;
constexpr uint32_t kListeningOnsetDecayMs = 280;

float Clamp(float value, float minimum, float maximum) {
    return std::max(minimum, std::min(maximum, value));
}

float ExponentialApproach(float current, float target, uint32_t elapsed_ms,
                          uint32_t time_constant_ms) {
    if (elapsed_ms == 0) return current;
    if (time_constant_ms == 0) return target;
    const float alpha = 1.0f - std::exp(
        -static_cast<float>(elapsed_ms) / static_cast<float>(time_constant_ms));
    return current + (target - current) * Clamp(alpha, 0.0f, 1.0f);
}

float SmoothStep(float value) {
    const float normalized = Clamp(value, 0.0f, 1.0f);
    return normalized * normalized * (3.0f - 2.0f * normalized);
}

float SegmentPulse(float seconds, float start, float end) {
    if (seconds <= start || seconds >= end) return 0.0f;
    const float progress = (seconds - start) / (end - start);
    const float wave = std::sin(progress * kPi);
    return wave * wave;
}

float EllipseField(float x, float y, float center_x, float center_y,
                   float radius_x, float radius_y) {
    const float dx = (x - center_x) / std::max(radius_x, 0.01f);
    const float dy = (y - center_y) / std::max(radius_y, 0.01f);
    return std::sqrt(dx * dx + dy * dy) - 1.0f;
}

float HappyArcField(float x, float y, float center_x, float center_y,
                    float width, float thickness) {
    const float dx = x - center_x;
    const float curve_y = center_y - 2.2f * kGridScale +
                          (dx * dx) / (width * 2.8f);
    return std::max(std::abs(dx) / width - 1.0f,
                    std::abs(y - curve_y) / thickness - 1.0f);
}

bool InsideEllipse(float x, float y, float center_x, float center_y,
                   float radius_x, float radius_y) {
    return EllipseField(x, y, center_x, center_y, radius_x, radius_y) <= 0.0f;
}

bool InsideSleepArc(float x, float y, float center_x, float center_y,
                    float width, float thickness) {
    const float dx = x - center_x;
    if (std::abs(dx) > width) return false;
    const float curve_y = center_y + 2.2f * kGridScale -
                          (dx * dx) / (width * 2.8f);
    return std::abs(y - curve_y) <= thickness;
}

bool InsideRing(float x, float y, float center_x, float center_y,
                float radius_x, float radius_y, float thickness) {
    return InsideEllipse(x, y, center_x, center_y, radius_x, radius_y) &&
           !InsideEllipse(x, y, center_x, center_y,
                          std::max(0.1f, radius_x - thickness),
                          std::max(0.1f, radius_y - thickness));
}

bool InsideDiamond(float x, float y, float center_x, float center_y,
                   float radius) {
    return std::abs(x - center_x) + std::abs(y - center_y) <= radius;
}

bool InsideLineSegment(float x, float y, float x1, float y1, float x2,
                       float y2, float thickness) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float length_squared = dx * dx + dy * dy;
    const float amount = length_squared <= 0.0f
                             ? 0.0f
                             : Clamp(((x - x1) * dx + (y - y1) * dy) /
                                         length_squared,
                                     0.0f, 1.0f);
    const float nearest_x = x1 + dx * amount;
    const float nearest_y = y1 + dy * amount;
    const float distance_x = x - nearest_x;
    const float distance_y = y - nearest_y;
    return std::sqrt(distance_x * distance_x + distance_y * distance_y) <=
           thickness;
}

bool GetMaskBit(const std::array<uint8_t, (56 * 56 + 7) / 8>& mask,
                int index) {
    return (mask[static_cast<size_t>(index) >> 3] &
            (1U << (index & 7))) != 0;
}

void SetMaskBit(std::array<uint8_t, (56 * 56 + 7) / 8>& mask, int index) {
    mask[static_cast<size_t>(index) >> 3] |=
        static_cast<uint8_t>(1U << (index & 7));
}

uint8_t* AllocateExpressionBuffer(size_t size) {
    constexpr size_t kAlignment = 64;
    auto* buffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        kAlignment, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        buffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            kAlignment, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return buffer;
}

}  // namespace

ExpressionPlayer::ExpressionPlayer(
    lv_obj_t* parent,
    audio::ListeningAudioFeatureStore* listening_audio_features)
    : parent_(parent), listening_audio_features_(listening_audio_features) {
    if (parent_ == nullptr || !lv_obj_is_valid(parent_)) return;

    lv_obj_add_event_cb(parent_, ParentDeletedCallback, LV_EVENT_DELETE, this);
    constexpr size_t kBufferSize = kExpressionWidth * kExpressionHeight;
    a8_buffer_ = AllocateExpressionBuffer(kBufferSize);
    if (a8_buffer_ == nullptr) {
        ESP_LOGE(kTag, "Unable to allocate %u-byte A8 expression buffer",
                 static_cast<unsigned>(kBufferSize));
        return;
    }
    auto* morph_memory = static_cast<uint8_t*>(heap_caps_malloc(
        kFieldCells * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (morph_memory == nullptr) {
        morph_memory = static_cast<uint8_t*>(heap_caps_malloc(
            kFieldCells * 3, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (morph_memory != nullptr) {
        morph_from_field_ = reinterpret_cast<int8_t*>(morph_memory);
        morph_to_field_ = reinterpret_cast<int8_t*>(morph_memory + kFieldCells);
        distance_scratch_ = morph_memory + kFieldCells * 2;
    } else {
        ESP_LOGW(kTag, "Direct expression morph disabled: no field memory");
    }
    std::memset(a8_buffer_, 0, kBufferSize);
    RegisterExpressionA8Buffer(a8_buffer_, kBufferSize);

    image_descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_descriptor_.header.cf = LV_COLOR_FORMAT_A8;
    image_descriptor_.header.w = kExpressionWidth;
    image_descriptor_.header.h = kExpressionHeight;
    image_descriptor_.header.stride = kExpressionWidth;
    image_descriptor_.data_size = kBufferSize;
    image_descriptor_.data = a8_buffer_;

    image_ = lv_image_create(parent_);
    lv_image_set_src(image_, &image_descriptor_);
    lv_obj_set_size(image_, kExpressionWidth, kExpressionHeight);
    lv_image_set_inner_align(image_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(image_);
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_image_recolor(
        image_, lv_color_hex(Theme::Get().colors().accent), LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(image_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_update_layout(image_);

    action_started_ms_ = lv_tick_get();
    motion_started_ms_ = action_started_ms_;
    UpdateFrame();
    frame_timer_ = lv_timer_create(FrameTimerCallback,
                                   expression_spec::kFrameDelayMs, this);
    ScheduleAmbient();
}

ExpressionPlayer::~ExpressionPlayer() { Stop(); }

void ExpressionPlayer::ClearListeningAudio() {
    if (listening_audio_features_ != nullptr) {
        listening_audio_features_->ClearOnset();
    }
    listening_activity_ = 0.0f;
    listening_vad_ = 0.0f;
    listening_onset_ = 0.0f;
    listening_audio_last_ms_ = lv_tick_get();
    listening_vad_hold_until_ms_ = 0;
}

void ExpressionPlayer::UpdateListeningAudio(uint32_t now) {
    if (action_ != Action::Listening || rendering_paused_) {
        ClearListeningAudio();
        return;
    }

    uint32_t elapsed_ms = listening_audio_last_ms_ == 0
                              ? kListeningAudioFrameMs
                              : lv_tick_elaps(listening_audio_last_ms_);
    // A long scheduling gap should not turn one stale frame into a visual
    // jump.  The next frame continues from the same time-constant response.
    elapsed_ms = std::min<uint32_t>(elapsed_ms, 250);
    listening_audio_last_ms_ = now;

    audio::ListeningAudioFeatures features;
    if (listening_audio_features_ != nullptr) {
        features = listening_audio_features_->ReadLatest();
    }
    const float activity_target = Clamp(features.activity, 0.0f, 1.0f);
    listening_activity_ = ExponentialApproach(
        listening_activity_, activity_target, elapsed_ms,
        activity_target > listening_activity_ ? kListeningActivityAttackMs
                                                : kListeningActivityReleaseMs);

    if (features.speech) {
        listening_vad_hold_until_ms_ = now + kListeningVadHoldMs;
    }
    const bool held_vad = features.speech ||
                          static_cast<int32_t>(listening_vad_hold_until_ms_ - now) > 0;
    const float vad_target = held_vad ? 1.0f : 0.0f;
    listening_vad_ = ExponentialApproach(
        listening_vad_, vad_target, elapsed_ms,
        vad_target > listening_vad_ ? kListeningVadAttackMs
                                     : kListeningVadReleaseMs);

    if (features.onset) listening_onset_ = 1.0f;
    else listening_onset_ = ExponentialApproach(
        listening_onset_, 0.0f, elapsed_ms, kListeningOnsetDecayMs);
}

void ExpressionPlayer::SetState(AgentState state) {
    const AgentState previous_state = state_;
    state_ = state;
    const Action desired = StateAction();
    if (desired != Action::Listening) ClearListeningAudio();
    else if (action_ != Action::Listening) ClearListeningAudio();
    if (sleeping_ || special_expression_held_) return;
    if (action_ == Action::Wake) {
        queued_action_ = desired;
        has_queued_action_ = true;
        return;
    }
    if (previous_state == state && action_ == desired && !morph_active_) return;
    CancelAmbient();
    if (previous_state == AgentState::Answering && state == AgentState::Idle) {
        PlayComplete();
        return;
    }
    RequestAction(desired);
}

void ExpressionPlayer::SetEnergy(Energy energy) {
    if (energy_ == energy) return;
    const uint32_t now = lv_tick_get();
    const float target_seconds = morph_active_ ? morph_target_seconds_
                                                : CurrentActionSeconds(now);
    energy_ = energy;
    if (has_rendered_) {
        BeginDirectMorph(action_, target_seconds, energy_);
    }
}

void ExpressionPlayer::SetLookAt(float x, float y) {
    target_look_x_ = Clamp(x, -1.0f, 1.0f);
    target_look_y_ = Clamp(y, -1.0f, 1.0f);
    tracking_active_ = true;
    tracking_release_pending_ = false;
}

void ExpressionPlayer::ClearLookAt() {
    target_look_x_ = 0.0f;
    target_look_y_ = 0.0f;
    tracking_release_pending_ = true;
}

void ExpressionPlayer::PlayBootAnimation() {
    state_ = AgentState::Idle;
    CancelAmbient();
    RequestAction(Action::Smile);
}

void ExpressionPlayer::PlayCharging() { RequestSpecial(Action::Charging); }

void ExpressionPlayer::PlayComplete() { RequestSpecial(Action::Complete); }

void ExpressionPlayer::PlayDizzy() { RequestSpecial(Action::Dizzy); }

void ExpressionPlayer::HoldCharging() { HoldSpecial(Action::Charging); }

void ExpressionPlayer::HoldDizzy() { HoldSpecial(Action::Dizzy); }

void ExpressionPlayer::ReleaseSpecialExpression() {
    if (!special_expression_held_) return;
    special_expression_held_ = false;
    held_special_action_ = Action::Idle;
    RequestAction(StateAction());
}

void ExpressionPlayer::Sleep() {
    if (sleeping_ && action_ == Action::Sleep) return;
    ClearListeningAudio();
    sleeping_ = true;
    has_queued_action_ = false;
    CancelAmbient();
    RequestAction(Action::Sleep);
}

void ExpressionPlayer::Wake() {
    if (!sleeping_ && action_ != Action::Sleep) return;
    ClearListeningAudio();
    sleeping_ = false;
    queued_action_ = StateAction();
    has_queued_action_ = true;
    CancelAmbient();
    BeginDirectMorph(Action::Wake, 0.0f, energy_);
}

void ExpressionPlayer::SetRenderingPaused(bool paused) {
    if (rendering_paused_ == paused) return;
    rendering_paused_ = paused;
    if (paused) {
        ClearListeningAudio();
        CancelAmbient();
        if (frame_timer_ != nullptr) lv_timer_pause(frame_timer_);
        return;
    }
    if (frame_timer_ != nullptr) lv_timer_resume(frame_timer_);
    ScheduleAmbient();
}

void ExpressionPlayer::SetWakeCompletedCallback(
    WakeCompletedCallback callback, void* user_data) {
    wake_completed_callback_ = callback;
    wake_completed_user_data_ = user_data;
}

void ExpressionPlayer::DispatchWakeCompleted() {
    WakeCompletedCallback callback = wake_completed_callback_;
    void* user_data = wake_completed_user_data_;
    wake_completed_callback_ = nullptr;
    wake_completed_user_data_ = nullptr;
    if (callback != nullptr) lv_async_call(callback, user_data);
}

bool ExpressionPlayer::IsWaking() const { return action_ == Action::Wake; }

void ExpressionPlayer::ActivateAction(Action action, uint32_t now,
                                      float seconds) {
    if (action != Action::Listening) ClearListeningAudio();
    action_ = action;
    morph_active_ = false;
    action_started_ms_ = now;
    action_elapsed_offset_ms_ = static_cast<uint32_t>(
        std::max(0.0f, seconds) * 1000.0f);
    if (action_ == Action::Idle && state_ == AgentState::Idle) ScheduleAmbient();
}

void ExpressionPlayer::RequestAction(Action action) {
    if (action != Action::Listening) ClearListeningAudio();
    if (sleeping_ && action != Action::Sleep && action != Action::Wake) {
        queued_action_ = action;
        has_queued_action_ = true;
        return;
    }
    if (action_ == action && !IsOneShot(action) && !morph_active_) return;
    BeginDirectMorph(action, TransitionTargetSeconds(action), energy_);
}

void ExpressionPlayer::RequestSpecial(Action action) {
    if (action_ == Action::Wake) {
        queued_action_ = action;
        has_queued_action_ = true;
        return;
    }
    ClearListeningAudio();
    CancelAmbient();
    RequestAction(action);
}

void ExpressionPlayer::HoldSpecial(Action action) {
    special_expression_held_ = true;
    held_special_action_ = action;
    RequestSpecial(action);
}

float ExpressionPlayer::CurrentActionSeconds(uint32_t now) const {
    if (morph_active_) return morph_target_seconds_;
    return static_cast<float>(action_elapsed_offset_ms_ +
                              lv_tick_elaps(action_started_ms_)) /
           1000.0f;
}

ExpressionPlayer::Action ExpressionPlayer::StateAction() const {
    switch (state_) {
        case AgentState::Connecting:
        case AgentState::Listening:
            return Action::Listening;
        case AgentState::Answering:
            return Action::Answering;
        case AgentState::Idle:
        default:
            return Action::Idle;
    }
}

float ExpressionPlayer::ActionDurationSeconds(Action action) const {
    return expression_spec::DurationSeconds(action);
}

bool ExpressionPlayer::IsOneShot(Action action) const {
    return ActionDurationSeconds(action) > 0.0f;
}

float ExpressionPlayer::TransitionTargetSeconds(Action action) const {
    if (action == Action::Listening) return 0.8f;
    if (action == Action::Answering) return 0.7f;
    if (action == Action::Idle || action == Action::Sleep ||
        action == Action::Wake) {
        return 0.0f;
    }
    return ActionDurationSeconds(action) * 0.5f;
}

void ExpressionPlayer::DistanceToMaskValue(
    const std::array<uint8_t, kMaskBytes>& mask, bool value) {
    if (distance_scratch_ == nullptr) return;
    constexpr uint8_t kFar = 126;
    for (int index = 0; index < static_cast<int>(kFieldCells); ++index) {
        distance_scratch_[index] = GetMaskBit(mask, index) == value ? 0 : kFar;
    }
    for (int y = 0; y < kGridSize; ++y) {
        for (int x = 0; x < kGridSize; ++x) {
            const int index = y * kGridSize + x;
            uint8_t best = distance_scratch_[index];
            if (x > 0) best = std::min<uint8_t>(
                best, static_cast<uint8_t>(
                          std::min<int>(kFar, distance_scratch_[index - 1] + 1)));
            if (y > 0) best = std::min<uint8_t>(
                best, static_cast<uint8_t>(std::min<int>(
                          kFar, distance_scratch_[index - kGridSize] + 1)));
            distance_scratch_[index] = best;
        }
    }
    for (int y = kGridSize - 1; y >= 0; --y) {
        for (int x = kGridSize - 1; x >= 0; --x) {
            const int index = y * kGridSize + x;
            uint8_t best = distance_scratch_[index];
            if (x + 1 < kGridSize) best = std::min<uint8_t>(
                best, static_cast<uint8_t>(
                          std::min<int>(kFar, distance_scratch_[index + 1] + 1)));
            if (y + 1 < kGridSize) best = std::min<uint8_t>(
                best, static_cast<uint8_t>(std::min<int>(
                          kFar, distance_scratch_[index + kGridSize] + 1)));
            distance_scratch_[index] = best;
        }
    }
}

void ExpressionPlayer::BuildSignedDistanceField(
    const std::array<uint8_t, kMaskBytes>& mask, int8_t* output) {
    if (output == nullptr || distance_scratch_ == nullptr) return;
    DistanceToMaskValue(mask, false);
    for (int index = 0; index < static_cast<int>(kFieldCells); ++index) {
        if (GetMaskBit(mask, index)) {
            output[index] = static_cast<int8_t>(distance_scratch_[index]);
        }
    }
    DistanceToMaskValue(mask, true);
    for (int index = 0; index < static_cast<int>(kFieldCells); ++index) {
        if (!GetMaskBit(mask, index)) {
            output[index] = -static_cast<int8_t>(distance_scratch_[index]);
        }
    }
}

void ExpressionPlayer::BuildMorphMask(float progress) {
    next_mask_.fill(0);
    const float amount = SmoothStep(progress);
    for (int index = 0; index < static_cast<int>(kFieldCells); ++index) {
        const float from = static_cast<float>(morph_from_field_[index]);
        const float value = from +
                            (static_cast<float>(morph_to_field_[index]) - from) *
                                amount;
        if (value >= 0.0f) SetMaskBit(next_mask_, index);
    }
}

void ExpressionPlayer::BeginDirectMorph(Action action, float target_seconds,
                                        Energy energy) {
    const uint32_t now = lv_tick_get();
    action_ = action;
    energy_ = energy;
    morph_target_seconds_ = std::max(0.0f, target_seconds);
    if (!has_rendered_ || morph_from_field_ == nullptr ||
        morph_to_field_ == nullptr || distance_scratch_ == nullptr) {
        ActivateAction(action, now, morph_target_seconds_);
        return;
    }
    const float motion_seconds =
        static_cast<float>(lv_tick_elaps(motion_started_ms_)) / 1000.0f;
    BuildMask(BuildFrameGeometry(morph_target_seconds_, motion_seconds, 1.0f),
              target_mask_);
    BuildSignedDistanceField(previous_mask_, morph_from_field_);
    BuildSignedDistanceField(target_mask_, morph_to_field_);
    morph_started_ms_ = now;
    morph_active_ = true;
}

void ExpressionPlayer::ScheduleAmbient() {
    if (ambient_timer_ != nullptr || state_ != AgentState::Idle ||
        action_ != Action::Idle || sleeping_ || morph_active_ ||
        rendering_paused_ || parent_ == nullptr) {
        return;
    }
    const uint32_t delay = kIdleDelayMinMs + (esp_random() % kIdleDelayRangeMs);
    ambient_timer_ = lv_timer_create(AmbientTimerCallback, delay, this);
    lv_timer_set_repeat_count(ambient_timer_, 1);
}

void ExpressionPlayer::CancelAmbient() {
    if (ambient_timer_ == nullptr) return;
    lv_timer_delete(ambient_timer_);
    ambient_timer_ = nullptr;
}

void ExpressionPlayer::PlayAmbient() {
    ambient_timer_ = nullptr;
    if (state_ != AgentState::Idle || action_ != Action::Idle || sleeping_) return;
    constexpr Action kAmbientActions[] = {
        Action::Smile,
        Action::Laugh,
        Action::Yawn,
    };
    RequestAction(kAmbientActions[esp_random() % 3]);
}

float ExpressionPlayer::ActionEnvelope(Action action, float seconds) const {
    if (action == Action::Idle) return 0.0f;
    if (action == Action::Sleep) return 1.0f;
    const float duration = expression_spec::EnvelopeSeconds(action);
    const bool loop = expression_spec::IsLooping(action) ||
                      (special_expression_held_ && action == held_special_action_);
    if (duration <= 0.0f) return 0.0f;
    float progress = loop ? std::fmod(std::max(seconds, 0.0f), duration) / duration
                          : Clamp(seconds / duration, 0.0f, 1.0f);
    const float wave = std::sin(progress * kPi);
    return wave * wave;
}

ExpressionPlayer::FrameGeometry ExpressionPlayer::BuildFrameGeometry(
    float seconds, float motion_seconds, float intensity_scale) const {
    FrameGeometry geometry;
    const bool tracking_expression =
        tracking_active_ && (action_ == Action::Idle ||
                             action_ == Action::Listening ||
                             action_ == Action::Answering);
    const Action rendered_action = tracking_expression ? Action::Idle : action_;
    geometry.action = rendered_action;
    geometry.seconds = seconds;
    if (energy_ == Energy::Exhausted) {
        geometry.energy_vertical_scale = 0.38f;
        geometry.energy_sag = 2.3f * kGridScale;
    } else if (energy_ == Energy::Tired) {
        geometry.energy_vertical_scale = 0.68f;
        geometry.energy_sag = 1.15f * kGridScale;
    }

    const float motion_phase =
        std::fmod(std::max(motion_seconds, 0.0f),
                  expression_spec::kMotionDurationSeconds) /
        expression_spec::kMotionDurationSeconds;
    float from_phase = 0.0f;
    float to_phase = 0.42f;
    float from_x = 0.0f;
    float from_y = 0.0f;
    float from_rotation = 0.0f;
    float to_x = 0.2f * kGridScale;
    float to_y = -0.8f * kGridScale;
    float to_rotation = 0.9f;
    if (motion_phase > 0.72f) {
        from_phase = 0.72f;
        to_phase = 1.0f;
        from_x = -0.2f * kGridScale;
        from_y = 0.8f * kGridScale;
        from_rotation = -0.9f;
        to_x = 0.0f;
        to_y = 0.0f;
        to_rotation = 0.0f;
    } else if (motion_phase > 0.42f) {
        from_phase = 0.42f;
        to_phase = 0.72f;
        from_x = 0.2f * kGridScale;
        from_y = -0.8f * kGridScale;
        from_rotation = 0.9f;
        to_x = -0.2f * kGridScale;
        to_y = 0.8f * kGridScale;
        to_rotation = -0.9f;
    }
    const float motion_amount = SmoothStep(
        (motion_phase - from_phase) / (to_phase - from_phase));
    geometry.translate_x = from_x + (to_x - from_x) * motion_amount;
    geometry.translate_y = from_y + (to_y - from_y) * motion_amount;
    const float rotation = from_rotation +
                           (to_rotation - from_rotation) * motion_amount;
    const float inverse_radians = -rotation * kPi / 180.0f;
    geometry.inverse_cos = std::cos(inverse_radians);
    geometry.inverse_sin = std::sin(inverse_radians);
    if (tracking_active_) {
        geometry.translate_x = 0.0f;
        geometry.translate_y = 0.0f;
        geometry.inverse_cos = 1.0f;
        geometry.inverse_sin = 0.0f;
    }

    float blink = 0.0f;
    float left_wink = 0.0f;
    float right_wink = 0.0f;
    float idle_look_x = 0.0f;
    if (rendered_action == Action::Idle) {
        const float idle_phase = std::fmod(std::max(motion_seconds, 0.0f), 12.0f);
        blink = std::max(SegmentPulse(idle_phase, 2.4f, 3.0f),
                         SegmentPulse(idle_phase, 10.6f, 11.15f));
        if (!tracking_active_) {
            left_wink = SegmentPulse(idle_phase, 7.15f, 8.05f);
            right_wink = SegmentPulse(idle_phase, 8.25f, 9.15f);
            idle_look_x = (SegmentPulse(idle_phase, 5.55f, 6.95f) -
                           SegmentPulse(idle_phase, 4.15f, 5.55f)) *
                          0.82f;
        }
    }

    const float breath = tracking_active_
                             ? 0.0f
                             : std::sin(motion_seconds * kTau /
                                        expression_spec::kMotionDurationSeconds);
    const float base_radius = (8.0f + breath * 0.2f) * kGridScale;
    const float action_amount =
        ActionEnvelope(rendered_action, seconds) * intensity_scale;
    const float manual_x_scale = tracking_active_ ? 3.25f : 1.25f;
    const float manual_y_scale = tracking_active_ ? 2.75f : 1.2f;
    const float look_offset_x =
        look_x_ * manual_x_scale + idle_look_x * 1.25f;
    geometry.left_x = (14.0f + look_offset_x) * kGridScale;
    geometry.right_x = (34.0f + look_offset_x) * kGridScale;
    geometry.left_y = (24.0f + look_y_ * manual_y_scale) * kGridScale;
    geometry.right_y = geometry.left_y;
    geometry.left_radius_x = base_radius;
    geometry.right_radius_x = base_radius;
    geometry.left_radius_y = base_radius;
    geometry.right_radius_y = base_radius;

    switch (rendered_action) {
        case Action::Idle:
            geometry.left_radius_y =
                base_radius * (1.0f - std::max(blink, left_wink) * 0.82f);
            geometry.right_radius_y =
                base_radius * (1.0f - std::max(blink, right_wink) * 0.82f);
            break;
        case Action::Smile: {
            const float lift = action_amount * 0.8f * kGridScale;
            geometry.left_y -= lift;
            geometry.right_y -= lift;
            geometry.left_arc_width = geometry.right_arc_width = 7.2f * kGridScale;
            geometry.left_arc_thickness = geometry.right_arc_thickness =
                1.05f * kGridScale;
            geometry.left_morph = geometry.right_morph = SmoothStep(action_amount);
            break;
        }
        case Action::Laugh: {
            const float bounce = std::abs(std::sin(seconds * kTau * 2.2f)) *
                                 1.2f * kGridScale * action_amount;
            const float thickness =
                (1.15f + std::abs(std::sin(seconds * kTau * 1.7f)) * 0.55f) *
                kGridScale;
            geometry.left_y -= bounce;
            geometry.right_y -= bounce;
            geometry.left_arc_width = geometry.right_arc_width = 7.5f * kGridScale;
            geometry.left_arc_thickness = geometry.right_arc_thickness = thickness;
            geometry.left_morph = geometry.right_morph = SmoothStep(action_amount);
            break;
        }
        case Action::Yawn: {
            const float openness = action_amount;
            geometry.left_radius_x = geometry.right_radius_x =
                base_radius + openness * 0.5f * kGridScale;
            geometry.left_radius_y = geometry.right_radius_y =
                base_radius + openness * 2.4f * kGridScale;
            break;
        }
        case Action::Listening: {
            const float wave = 0.5f + std::sin(seconds * kTau * 1.15f) * 0.5f;
            const float attention =
                std::min(1.0f, listening_vad_ * 0.62f +
                                  listening_onset_ * 0.9f);
            const float elastic =
                listening_onset_ * std::sin(seconds * kTau * 2.2f) * 0.65f;
            const float audio_pulse = listening_activity_ *
                (0.9f + 0.3f * std::sin(seconds * kTau * 1.15f + 0.55f));
            const float radius_scale = 1.0f - attention * 0.08f;
            const float attentive_left_x =
                geometry.left_x + attention * 0.85f * kGridScale;
            const float attentive_right_x =
                geometry.right_x - attention * 0.85f * kGridScale;
            geometry.left_x = attentive_left_x;
            geometry.right_x = attentive_right_x;
            geometry.left_radius_x = geometry.left_radius_y =
                (base_radius +
                 (wave - 0.5f) * 3.0f * action_amount * 0.42f * kGridScale +
                 (audio_pulse + elastic) * kGridScale) * radius_scale;
            geometry.right_radius_x = geometry.right_radius_y =
                (base_radius +
                 (0.5f - wave) * 3.0f * action_amount * 0.42f * kGridScale +
                 (audio_pulse - elastic) * kGridScale) * radius_scale;
            const float bob =
                (wave - 0.5f) * 1.8f * 0.42f * kGridScale * action_amount +
                listening_onset_ * std::sin(seconds * kTau * 1.55f) *
                    0.55f * kGridScale;
            geometry.left_y -= bob;
            geometry.right_y += bob;
            break;
        }
        case Action::Answering: {
            const int variant = static_cast<int>(std::floor(std::max(seconds, 0.0f) /
                                                            1.4f)) %
                                3;
            const float talk_pulse = 0.82f + std::sin(seconds * kTau * 2.4f) * 0.18f;
            const float smile_amount = SmoothStep(action_amount) * talk_pulse;
            const float bounce = variant == 1
                                     ? std::abs(std::sin(seconds * kTau * 2.1f)) *
                                           0.45f * action_amount
                                     : 0.0f;
            constexpr float kLeftLift[] = {0.9f, 1.35f, 0.65f};
            constexpr float kRightLift[] = {0.9f, 1.35f, 1.25f};
            constexpr float kLeftWidth[] = {7.5f, 8.2f, 7.0f};
            constexpr float kRightWidth[] = {7.5f, 8.2f, 8.0f};
            constexpr float kLeftThickness[] = {1.05f, 1.35f, 1.0f};
            constexpr float kRightThickness[] = {1.05f, 1.35f, 1.45f};
            geometry.left_y -= (kLeftLift[variant] + bounce) * action_amount * kGridScale;
            geometry.right_y -= (kRightLift[variant] + bounce) * action_amount * kGridScale;
            geometry.left_arc_width = kLeftWidth[variant] * kGridScale;
            geometry.right_arc_width = kRightWidth[variant] * kGridScale;
            geometry.left_arc_thickness =
                (kLeftThickness[variant] + action_amount * 0.4f) * kGridScale;
            geometry.right_arc_thickness =
                (kRightThickness[variant] + action_amount * 0.4f) * kGridScale;
            geometry.left_morph = geometry.right_morph = smile_amount;
            break;
        }
        case Action::Charging: {
            const float pulse = SmoothStep(action_amount);
            geometry.left_radius_x = geometry.left_radius_y =
                base_radius + pulse * 1.5f * kGridScale;
            geometry.right_radius_x = geometry.right_radius_y =
                geometry.left_radius_x;
            break;
        }
        case Action::Complete: {
            const float smile_amount = SmoothStep(action_amount);
            geometry.left_y -= smile_amount * kGridScale;
            geometry.right_y -= smile_amount * kGridScale;
            geometry.left_arc_width = geometry.right_arc_width =
                7.4f * kGridScale;
            geometry.left_arc_thickness = geometry.right_arc_thickness =
                1.2f * kGridScale;
            geometry.left_morph = geometry.right_morph = smile_amount;
            break;
        }
        case Action::Wake: {
            float open = 1.0f;
            if (seconds < 0.28f) {
                open = SmoothStep(seconds / 0.28f) * 1.18f;
            } else if (seconds < 0.5f) {
                open = 1.18f + (0.12f - 1.18f) *
                                   SmoothStep((seconds - 0.28f) / 0.22f);
            } else if (seconds < 0.78f) {
                open = 0.12f + 0.88f *
                                   SmoothStep((seconds - 0.5f) / 0.28f);
            }
            geometry.left_radius_y = geometry.right_radius_y =
                (0.7f + 7.3f * open) * kGridScale;
            break;
        }
        case Action::Sleep:
        case Action::Dizzy:
            break;
    }
    return geometry;
}

bool ExpressionPlayer::IsDotActive(float x, float y,
                                   const FrameGeometry& geometry) const {
    const float translated_x = x - kPivot - geometry.translate_x;
    const float translated_y = y - kPivot - geometry.translate_y;
    const float sample_x = kPivot + translated_x * geometry.inverse_cos -
                           translated_y * geometry.inverse_sin;
    const float symbol_y = kPivot + translated_x * geometry.inverse_sin +
                           translated_y * geometry.inverse_cos;

    auto energy_y = [&](float center_y) {
        return center_y +
               (symbol_y - center_y - geometry.energy_sag) /
                   geometry.energy_vertical_scale;
    };

    auto inside_eye = [&](float center_x, float center_y, float radius_x,
                          float radius_y, float arc_width, float arc_thickness,
                          float morph) {
        const float sample_y = energy_y(center_y);
        const float ellipse = EllipseField(sample_x, sample_y, center_x, center_y,
                                           radius_x, radius_y);
        if (morph <= 0.0f) return ellipse <= 0.0f;
        const float smile = HappyArcField(sample_x, sample_y, center_x, center_y,
                                          arc_width, arc_thickness);
        return ellipse + (smile - ellipse) * morph <= 0.0f;
    };

    const float seconds = geometry.seconds;
    const float action_amount = ActionEnvelope(geometry.action, seconds);
    if (geometry.action == Action::Charging) {
        const float pulse = SmoothStep(action_amount);
        const float bolt_thickness = (0.7f + pulse * 0.45f) * kGridScale;
        return inside_eye(geometry.left_x, geometry.left_y,
                          geometry.left_radius_x, geometry.left_radius_y,
                          0.0f, 0.0f, 0.0f) ||
               inside_eye(geometry.right_x, geometry.right_y,
                          geometry.right_radius_x, geometry.right_radius_y,
                          0.0f, 0.0f, 0.0f) ||
               InsideLineSegment(sample_x, symbol_y, 47.0f * kGridScale,
                                 7.0f * kGridScale, 42.0f * kGridScale,
                                 13.0f * kGridScale, bolt_thickness) ||
               InsideLineSegment(sample_x, symbol_y, 42.0f * kGridScale,
                                 13.0f * kGridScale, 47.0f * kGridScale,
                                 13.0f * kGridScale, bolt_thickness) ||
               InsideLineSegment(sample_x, symbol_y, 47.0f * kGridScale,
                                 13.0f * kGridScale, 41.0f * kGridScale,
                                 21.0f * kGridScale, bolt_thickness);
    }

    if (geometry.action == Action::Complete) {
        const float sparkle = SegmentPulse(seconds, 0.2f, 1.3f);
        return inside_eye(geometry.left_x, geometry.left_y,
                          geometry.left_radius_x, geometry.left_radius_y,
                          geometry.left_arc_width, geometry.left_arc_thickness,
                          geometry.left_morph) ||
               inside_eye(geometry.right_x, geometry.right_y,
                          geometry.right_radius_x, geometry.right_radius_y,
                          geometry.right_arc_width, geometry.right_arc_thickness,
                          geometry.right_morph) ||
               InsideDiamond(sample_x, symbol_y, 45.0f * kGridScale,
                             10.0f * kGridScale,
                             3.2f * kGridScale * sparkle);
    }

    if (geometry.action == Action::Sleep) {
        const float drift = std::sin(seconds * kTau * 0.18f) *
                            0.45f * kGridScale;
        const bool left_sleeping = InsideSleepArc(
            sample_x, energy_y(geometry.left_y), geometry.left_x,
            geometry.left_y + drift, 7.2f * kGridScale, 0.9f * kGridScale);
        const bool right_sleeping = InsideSleepArc(
            sample_x, energy_y(geometry.right_y), geometry.right_x,
            geometry.right_y + drift, 7.2f * kGridScale, 0.9f * kGridScale);
        const float z_phase = std::fmod(std::max(seconds, 0.0f), 2.4f) / 2.4f;
        return left_sleeping || right_sleeping ||
               InsideDiamond(sample_x, energy_y(geometry.left_y),
                             (42.0f + z_phase * 4.0f) * kGridScale,
                             (18.0f - z_phase * 8.0f) * kGridScale,
                             1.35f * kGridScale);
    }

    if (geometry.action == Action::Dizzy) {
        const float orbit = seconds * kTau * 1.8f;
        const float wobble_x = std::sin(seconds * kTau * 3.6f) *
                               1.2f * kGridScale;
        const float ring_radius = (5.5f + action_amount * 1.3f) * kGridScale;
        const float orbit_radius = 4.2f * kGridScale;
        const float left_y = energy_y(geometry.left_y);
        const float right_y = energy_y(geometry.right_y);
        return InsideRing(sample_x, left_y, geometry.left_x + wobble_x,
                          geometry.left_y, ring_radius, ring_radius,
                          1.2f * kGridScale) ||
               InsideRing(sample_x, right_y, geometry.right_x - wobble_x,
                          geometry.right_y, ring_radius, ring_radius,
                          1.2f * kGridScale) ||
               InsideEllipse(sample_x, left_y,
                             geometry.left_x + std::cos(orbit) * orbit_radius,
                             geometry.left_y + std::sin(orbit) * orbit_radius,
                             1.3f * kGridScale, 1.3f * kGridScale) ||
               InsideEllipse(sample_x, right_y,
                             geometry.right_x - std::cos(orbit) * orbit_radius,
                             geometry.right_y - std::sin(orbit) * orbit_radius,
                             1.3f * kGridScale, 1.3f * kGridScale);
    }

    return inside_eye(geometry.left_x, geometry.left_y,
                      geometry.left_radius_x, geometry.left_radius_y,
                      geometry.left_arc_width, geometry.left_arc_thickness,
                      geometry.left_morph) ||
           inside_eye(geometry.right_x, geometry.right_y,
                      geometry.right_radius_x, geometry.right_radius_y,
                      geometry.right_arc_width, geometry.right_arc_thickness,
                      geometry.right_morph);
}

void ExpressionPlayer::BuildMask(
    const FrameGeometry& geometry,
    std::array<uint8_t, kMaskBytes>& mask) const {
    mask.fill(0);
    for (int y = 0; y < kGridSize; ++y) {
        for (int x = 0; x < kGridSize; ++x) {
            if (IsDotActive(static_cast<float>(x), static_cast<float>(y), geometry)) {
                SetMaskBit(mask, y * kGridSize + x);
            }
        }
    }
}

void ExpressionPlayer::InvalidateCellBounds(int min_x, int min_y, int max_x,
                                            int max_y) {
    if (image_ == nullptr || min_x > max_x || min_y > max_y) return;
    lv_area_t image_area;
    lv_obj_get_coords(image_, &image_area);
    lv_area_t dirty = {
        .x1 = image_area.x1 + min_x * kLogicalExtent / kGridSize,
        .y1 = image_area.y1 + std::max(
            0, kLogicalOriginY + min_y * kLogicalExtent / kGridSize),
        .x2 = image_area.x1 +
              std::min(kExpressionWidth,
                       (max_x + 1) * kLogicalExtent / kGridSize) -
              1,
        .y2 = image_area.y1 +
              std::min(kExpressionHeight,
                       kLogicalOriginY +
                           (max_y + 1) * kLogicalExtent / kGridSize) -
              1,
    };
    if (dirty.x1 > dirty.x2 || dirty.y1 > dirty.y2) return;
    lv_obj_invalidate_area(image_, &dirty);
}

void ExpressionPlayer::RedrawChangedCells() {
    int left_min_x = kGridSize;
    int left_min_y = kGridSize;
    int left_max_x = -1;
    int left_max_y = -1;
    int right_min_x = kGridSize;
    int right_min_y = kGridSize;
    int right_max_x = -1;
    int right_max_y = -1;
    constexpr int kDotSize = 7;

    for (int y = 0; y < kGridSize; ++y) {
        for (int x = 0; x < kGridSize; ++x) {
            const int index = y * kGridSize + x;
            const bool previous = GetMaskBit(previous_mask_, index);
            const bool next = GetMaskBit(next_mask_, index);
            if (previous == next) continue;

            int& min_x = x < kGridSize / 2 ? left_min_x : right_min_x;
            int& min_y = x < kGridSize / 2 ? left_min_y : right_min_y;
            int& max_x = x < kGridSize / 2 ? left_max_x : right_max_x;
            int& max_y = x < kGridSize / 2 ? left_max_y : right_max_y;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);

            const int raw_x1 = x * kLogicalExtent / kGridSize;
            const int raw_y1 =
                kLogicalOriginY + y * kLogicalExtent / kGridSize;
            const int raw_x2 = (x + 1) * kLogicalExtent / kGridSize;
            const int raw_y2 =
                kLogicalOriginY + (y + 1) * kLogicalExtent / kGridSize;
            const int pixel_x1 = std::max(0, raw_x1);
            const int pixel_y1 = std::max(0, raw_y1);
            const int pixel_x2 = std::min(kExpressionWidth, raw_x2);
            const int pixel_y2 = std::min(kExpressionHeight, raw_y2);
            for (int py = pixel_y1; py < pixel_y2; ++py) {
                std::memset(a8_buffer_ + py * kExpressionWidth + pixel_x1, 0,
                            static_cast<size_t>(pixel_x2 - pixel_x1));
            }
            if (!next) continue;

            const int dot_x1 = std::max(0, (raw_x1 + raw_x2 - kDotSize) / 2);
            const int dot_y1 = std::max(0, (raw_y1 + raw_y2 - kDotSize) / 2);
            const int dot_x2 = std::min(kExpressionWidth, dot_x1 + kDotSize);
            const int dot_y2 = std::min(kExpressionHeight, dot_y1 + kDotSize);
            for (int py = dot_y1; py < dot_y2; ++py) {
                std::memset(a8_buffer_ + py * kExpressionWidth + dot_x1,
                            LV_OPA_COVER,
                            static_cast<size_t>(dot_x2 - dot_x1));
            }
        }
    }

    if (left_max_x >= 0 || right_max_x >= 0) {
        lv_image_cache_drop(&image_descriptor_);
    }
    InvalidateCellBounds(left_min_x, left_min_y, left_max_x, left_max_y);
    InvalidateCellBounds(right_min_x, right_min_y, right_max_x, right_max_y);
    previous_mask_.swap(next_mask_);
}

void ExpressionPlayer::UpdateFrame() {
    if (parent_ == nullptr || image_ == nullptr || a8_buffer_ == nullptr ||
        !lv_obj_is_valid(image_)) {
        return;
    }
    const uint32_t now = lv_tick_get();
    UpdateListeningAudio(now);
    look_x_ += (target_look_x_ - look_x_) * 0.18f;
    look_y_ += (target_look_y_ - look_y_) * 0.18f;
    if (tracking_release_pending_ && std::abs(look_x_) < 0.02f &&
        std::abs(look_y_) < 0.02f) {
        look_x_ = 0.0f;
        look_y_ = 0.0f;
        tracking_active_ = false;
        tracking_release_pending_ = false;
    }
    const float motion_seconds =
        static_cast<float>(lv_tick_elaps(motion_started_ms_)) / 1000.0f;

    if (morph_active_) {
        const float progress =
            static_cast<float>(lv_tick_elaps(morph_started_ms_)) /
            static_cast<float>(expression_spec::kDirectMorphMs);
        BuildMorphMask(Clamp(progress, 0.0f, 1.0f));
        RedrawChangedCells();
        has_rendered_ = true;
        if (progress >= 1.0f) {
            morph_active_ = false;
            action_started_ms_ = now;
            action_elapsed_offset_ms_ = static_cast<uint32_t>(
                morph_target_seconds_ * 1000.0f);
            if (action_ == Action::Idle && state_ == AgentState::Idle) {
                ScheduleAmbient();
            }
        }
        return;
    }

    float seconds = CurrentActionSeconds(now);
    const bool held_special =
        special_expression_held_ && action_ == held_special_action_;
    const float duration = held_special ? 0.0f : ActionDurationSeconds(action_);
    if (duration > 0.0f && seconds >= duration) {
        const bool completed_wake = action_ == Action::Wake;
        Action next = StateAction();
        if (has_queued_action_) {
            next = queued_action_;
            has_queued_action_ = false;
        }
        RequestAction(next);
        if (completed_wake) DispatchWakeCompleted();
        if (morph_active_) {
            BuildMorphMask(0.0f);
            RedrawChangedCells();
            has_rendered_ = true;
            return;
        }
        seconds = CurrentActionSeconds(now);
    }

    BuildMask(BuildFrameGeometry(seconds, motion_seconds, 1.0f), next_mask_);
    RedrawChangedCells();
    has_rendered_ = true;
}

void ExpressionPlayer::Stop() {
    ClearListeningAudio();
    CancelAmbient();
    wake_completed_callback_ = nullptr;
    wake_completed_user_data_ = nullptr;
    if (frame_timer_ != nullptr) {
        lv_timer_delete(frame_timer_);
        frame_timer_ = nullptr;
    }
    if (parent_ != nullptr && lv_obj_is_valid(parent_)) {
        lv_obj_remove_event_cb_with_user_data(parent_, ParentDeletedCallback, this);
    }
    if (image_ != nullptr && lv_obj_is_valid(image_)) lv_obj_delete(image_);
    image_ = nullptr;
    parent_ = nullptr;
    if (a8_buffer_ != nullptr) {
        lv_image_cache_drop(&image_descriptor_);
        UnregisterExpressionA8Buffer(a8_buffer_);
        heap_caps_free(a8_buffer_);
        a8_buffer_ = nullptr;
        image_descriptor_.data = nullptr;
    }
    if (morph_from_field_ != nullptr) {
        heap_caps_free(morph_from_field_);
        morph_from_field_ = nullptr;
        morph_to_field_ = nullptr;
        distance_scratch_ = nullptr;
    }
}

void ExpressionPlayer::FrameTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<ExpressionPlayer*>(lv_timer_get_user_data(timer));
    if (self == nullptr) return;
    const uint32_t desired_period =
        PerformanceManager::Get().animation_frame_period_ms();
    if (self->frame_period_ms_ != desired_period) {
        self->frame_period_ms_ = desired_period;
        lv_timer_set_period(timer, desired_period);
    }
    self->UpdateFrame();
}

void ExpressionPlayer::AmbientTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<ExpressionPlayer*>(lv_timer_get_user_data(timer));
    if (self != nullptr) self->PlayAmbient();
}

void ExpressionPlayer::ParentDeletedCallback(lv_event_t* event) {
    auto* self = static_cast<ExpressionPlayer*>(lv_event_get_user_data(event));
    if (self == nullptr) return;
    self->CancelAmbient();
    if (self->frame_timer_ != nullptr) {
        lv_timer_delete(self->frame_timer_);
        self->frame_timer_ = nullptr;
    }
    self->parent_ = nullptr;
    self->image_ = nullptr;
}

}  // namespace agent_ui
