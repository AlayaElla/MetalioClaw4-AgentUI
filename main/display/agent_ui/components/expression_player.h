#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "agent_ui_types.h"
#include "expression_spec.generated.h"
#include "listening_audio_features.h"
#include "lvgl.h"

namespace agent_ui {

class ExpressionPlayer {
public:
    using WakeCompletedCallback = void (*)(void* user_data);

    using Energy = expression_spec::Energy;

    explicit ExpressionPlayer(
        lv_obj_t* parent,
        audio::ListeningAudioFeatureStore* listening_audio_features = nullptr);
    ~ExpressionPlayer();

    void SetState(AgentState state);
    void SetEnergy(Energy energy);
    void SetLookAt(float x, float y);
    void ClearLookAt();
    void PlayBootAnimation();
    void PlayCharging();
    void PlayComplete();
    void PlayDizzy();
    void HoldCharging();
    void HoldDizzy();
    void ReleaseSpecialExpression();
    void Sleep();
    void Wake();
    void SetRenderingPaused(bool paused);
    void SetWakeCompletedCallback(WakeCompletedCallback callback,
                                  void* user_data);
    bool IsSleeping() const { return sleeping_; }
    bool IsWaking() const;
    void Stop();

private:
    using Action = expression_spec::Action;

    struct FrameGeometry {
        Action action = Action::Idle;
        float inverse_cos = 1.0f;
        float inverse_sin = 0.0f;
        float translate_x = 0.0f;
        float translate_y = 0.0f;
        float left_x = 0.0f;
        float right_x = 0.0f;
        float left_y = 0.0f;
        float right_y = 0.0f;
        float left_radius_x = 0.0f;
        float left_radius_y = 0.0f;
        float right_radius_x = 0.0f;
        float right_radius_y = 0.0f;
        float left_arc_width = 0.0f;
        float right_arc_width = 0.0f;
        float left_arc_thickness = 0.0f;
        float right_arc_thickness = 0.0f;
        float left_morph = 0.0f;
        float right_morph = 0.0f;
        float seconds = 0.0f;
        float energy_vertical_scale = 1.0f;
        float energy_sag = 0.0f;
    };

    static constexpr int kGridSize = 56;
    static constexpr int kExpressionWidth = 600;
    static constexpr int kExpressionHeight = 400;
    static constexpr int kLogicalExtent = 600;
    static constexpr int kLogicalOriginY =
        (kExpressionHeight - kLogicalExtent) / 2;
    static constexpr size_t kMaskBytes =
        (kGridSize * kGridSize + 7) / 8;
    static constexpr size_t kFieldCells = kGridSize * kGridSize;

    static void FrameTimerCallback(lv_timer_t* timer);
    static void AmbientTimerCallback(lv_timer_t* timer);
    static void ParentDeletedCallback(lv_event_t* event);

    void ActivateAction(Action action, uint32_t now, float seconds = 0.0f);
    void RequestAction(Action action);
    void RequestSpecial(Action action);
    void HoldSpecial(Action action);
    void BeginDirectMorph(Action action, float target_seconds, Energy energy);
    void BuildSignedDistanceField(
        const std::array<uint8_t, kMaskBytes>& mask, int8_t* output);
    void DistanceToMaskValue(
        const std::array<uint8_t, kMaskBytes>& mask, bool value);
    void BuildMorphMask(float progress);
    float CurrentActionSeconds(uint32_t now) const;
    Action StateAction() const;
    float TransitionTargetSeconds(Action action) const;
    float ActionDurationSeconds(Action action) const;
    bool IsOneShot(Action action) const;
    void ScheduleAmbient();
    void CancelAmbient();
    void PlayAmbient();
    void UpdateFrame();
    void UpdateListeningAudio(uint32_t now);
    void ClearListeningAudio();
    void BuildMask(const FrameGeometry& geometry,
                   std::array<uint8_t, kMaskBytes>& mask) const;
    void RedrawChangedCells();
    void InvalidateCellBounds(int min_x, int min_y, int max_x, int max_y);
    void DispatchWakeCompleted();

    FrameGeometry BuildFrameGeometry(float seconds, float motion_seconds,
                                     float intensity_scale) const;
    bool IsDotActive(float x, float y, const FrameGeometry& geometry) const;
    float ActionEnvelope(Action action, float seconds) const;

    lv_obj_t* parent_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_timer_t* frame_timer_ = nullptr;
    uint32_t frame_period_ms_ = expression_spec::kFrameDelayMs;
    lv_timer_t* ambient_timer_ = nullptr;
    uint8_t* a8_buffer_ = nullptr;
    lv_image_dsc_t image_descriptor_{};
    std::array<uint8_t, kMaskBytes> previous_mask_{};
    std::array<uint8_t, kMaskBytes> next_mask_{};
    std::array<uint8_t, kMaskBytes> target_mask_{};
    int8_t* morph_from_field_ = nullptr;
    int8_t* morph_to_field_ = nullptr;
    uint8_t* distance_scratch_ = nullptr;
    AgentState state_ = AgentState::Idle;
    Action action_ = Action::Idle;
    Action queued_action_ = Action::Idle;
    Energy energy_ = Energy::Normal;
    bool has_queued_action_ = false;
    bool special_expression_held_ = false;
    Action held_special_action_ = Action::Idle;
    bool has_rendered_ = false;
    bool morph_active_ = false;
    bool sleeping_ = false;
    uint32_t action_started_ms_ = 0;
    uint32_t motion_started_ms_ = 0;
    uint32_t action_elapsed_offset_ms_ = 0;
    uint32_t morph_started_ms_ = 0;
    float morph_target_seconds_ = 0.0f;
    float look_x_ = 0.0f;
    float look_y_ = 0.0f;
    float target_look_x_ = 0.0f;
    float target_look_y_ = 0.0f;
    bool tracking_active_ = false;
    bool tracking_release_pending_ = false;
    bool rendering_paused_ = false;
    audio::ListeningAudioFeatureStore* listening_audio_features_ = nullptr;
    float listening_activity_ = 0.0f;
    float listening_vad_ = 0.0f;
    float listening_onset_ = 0.0f;
    uint32_t listening_audio_last_ms_ = 0;
    uint32_t listening_vad_hold_until_ms_ = 0;
    WakeCompletedCallback wake_completed_callback_ = nullptr;
    void* wake_completed_user_data_ = nullptr;
};

}  // namespace agent_ui
