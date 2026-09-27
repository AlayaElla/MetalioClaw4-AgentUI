#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "agent_ui_types.h"
#include "expression_art.generated.h"
#include "expression_spec.generated.h"
#include "listening_audio_features.h"
#include "lvgl.h"

namespace agent_ui {

// Frames are emitted as flat polygons in expression-surface pixel space and
// rasterised with anti-aliasing, so the player never keeps a per-pixel buffer
// of its own beyond the A8 image it draws into.
namespace expression_geometry {

// A status glyph is a stack of traced loops, holes already bridged into their
// shell, so it needs more polygons than the two-polygon eye poses. Keep this in
// step with the tracer's MAX_GLYPH_SHAPES: a shape past either cap is dropped
// by AddPolygon() rather than growing the buffer.
inline constexpr int kMaxShapes = 12;
// A smoothed glyph outline needs more corners than a staircase one; the whole
// shape set is scratch allocated once per player, so this only costs PSRAM.
inline constexpr int kMaxShapePoints = 56;

struct Shape {
    int16_t count = 0;
    float x[kMaxShapePoints];
    float y[kMaxShapePoints];
    int16_t top = 0;
    int16_t bottom = -1;
};

struct ShapeSet {
    Shape shapes[kMaxShapes];
    int16_t count = 0;
    int16_t left = 0;
    int16_t top = 0;
    int16_t right = -1;
    int16_t bottom = -1;
};

// One eye of the current frame: an imported silhouette plus the procedural
// deformation applied on top of it. Scales are about the silhouette's own
// centroid, shifts are in logical units.
struct EyeState {
    const expression_art::Eye* eye = nullptr;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float shift_x = 0.0f;
    float shift_y = 0.0f;
};

}  // namespace expression_geometry

class ExpressionPlayer {
public:
    using WakeCompletedCallback = void (*)(void* user_data);

    using Energy = expression_spec::Energy;

    explicit ExpressionPlayer(
        lv_obj_t* parent,
        audio::ListeningAudioFeatureStore* listening_audio_features = nullptr,
        int surface_width = 600,
        int surface_height = 400,
        int pixel_step = 1);
    ~ExpressionPlayer();

    void SetState(AgentState state);
    void SetEnergy(Energy energy);
    void SetLookAt(float x, float y);
    void ClearLookAt();
    void PlayBootAnimation();
    void PlayComplete();
    void PlayDizzy();
    // Whole-panel status graphics. These replace the eye pair for the length of
    // the action, so they are announcements rather than held states.
    void PlayBattery();
    void PlayBatteryFull();
    void PlayBatteryLow();
    void HoldCharging();
    void HoldDizzy();
    void ReleaseSpecialExpression();
    void Sleep();
    void Wake();
    void SetRenderingPaused(bool paused);
    void SetWakeCompletedCallback(WakeCompletedCallback callback,
                                  void* user_data);
    bool IsSleeping() const { return sleeping_; }
    // True while a caller (the charging and dizzy AI interactions) keeps one
    // action pinned; a one-shot announcement can only borrow the surface until
    // the pin is released.
    bool HoldsSpecial() const { return special_expression_held_; }
    bool IsWaking() const;
    void Stop();

private:
    using Action = expression_spec::Action;

    struct FrameGeometry {
        Action action = Action::Idle;
        float seconds = 0.0f;
        float rotation = 0.0f;
        float translate_x = 0.0f;
        float translate_y = 0.0f;
        float energy_vertical_scale = 1.0f;
        float energy_sag = 0.0f;
        expression_geometry::EyeState left;
        expression_geometry::EyeState right;
        // Set when the action wears a whole-surface status graphic instead of an
        // eye pair; the two eye states are then unused.
        const expression_art::Glyph* glyph = nullptr;
        float glyph_scale = 1.0f;
        float symbol_amount = 0.0f;
    };

    static constexpr int kExpressionWidth = 600;
    static constexpr int kExpressionHeight = 400;

    static void FrameTimerCallback(lv_timer_t* timer);
    static void AmbientTimerCallback(lv_timer_t* timer);
    static void ParentDeletedCallback(lv_event_t* event);

    void ActivateAction(Action action, uint32_t now, float seconds = 0.0f);
    void RequestAction(Action action);
    void RequestSpecial(Action action);
    void HoldSpecial(Action action);
    void BeginDirectMorph(Action action, float target_seconds, Energy energy);
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
    void Render(float dissolve);
    void InvalidateRect(int left, int top, int right, int bottom);
    void DispatchWakeCompleted();

    FrameGeometry BuildFrameGeometry(float seconds, float motion_seconds,
                                     float intensity_scale) const;
    void Emit(const FrameGeometry& geometry,
              expression_geometry::ShapeSet& shapes) const;
    float ActionEnvelope(Action action, float seconds) const;

    lv_obj_t* parent_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_timer_t* frame_timer_ = nullptr;
    uint32_t frame_period_ms_ = expression_spec::kFrameDelayMs;
    lv_timer_t* ambient_timer_ = nullptr;
    uint8_t* a8_buffer_ = nullptr;
    lv_image_dsc_t image_descriptor_{};
    // The artwork is laid out in a 48-unit square, so one surface pixel per
    // logical unit is fixed by the width; the height only decides how much of
    // the band around the eyes is visible.
    int surface_width_ = kExpressionWidth;
    int surface_height_ = kExpressionHeight;
    uint32_t accent_ = 0;
    expression_geometry::ShapeSet* shape_sets_ = nullptr;
    int16_t* row_coverage_ = nullptr;
    expression_geometry::ShapeSet* current_shapes_ = nullptr;
    expression_geometry::ShapeSet* previous_shapes_ = nullptr;
    int16_t last_left_ = 0;
    int16_t last_top_ = 0;
    int16_t last_right_ = -1;
    int16_t last_bottom_ = -1;
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
