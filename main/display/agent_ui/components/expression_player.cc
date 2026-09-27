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

namespace art = expression_art;
namespace geo = expression_geometry;

constexpr char kTag[] = "ExpressionPlayer";
constexpr uint32_t kIdleDelayMinMs = 6000;
constexpr uint32_t kIdleDelayRangeMs = 8001;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = kPi * 2.0f;
// The artwork lives in a 48-unit square that spans the full expression width, so
// (24, 24) is the canvas centre and one logical unit is surface width / 48
// pixels. FaceTransform carries that mapping per player, which is what lets the
// status bar show a reduced copy of the same face.
constexpr float kLogicalCenter = art::kLogicalCenter;
constexpr int kSubSamples = 4;
constexpr int kCoveragePerSubSample = 256 / kSubSamples;
constexpr int kMaxCrossings = 2 * geo::kMaxShapePoints;
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

// Every pulse scales an eye about its own centre, so growing the eyes eats the
// gap between them and the pair reads as one slab. Pushing the eyes apart
// instead keeps the breathing motion while the silhouette stays two eyes.
void KeepEyesApart(geo::EyeState& left, geo::EyeState& right) {
    constexpr float kMinGap = art::kMinEyeGap;
    if (left.eye == nullptr || right.eye == nullptr) return;
    const float left_inner = left.eye->center[0] + left.shift_x +
                             left.eye->radius[0] * left.scale_x;
    const float right_inner = right.eye->center[0] + right.shift_x -
                              right.eye->radius[0] * right.scale_x;
    const float deficit = kMinGap - (right_inner - left_inner);
    if (deficit <= 0.0f) return;
    left.shift_x -= deficit * 0.5f;
    right.shift_x += deficit * 0.5f;
}

// Maps the 48-unit artwork square onto one player's A8 surface and carries that
// surface's extent, so every emitter below clips and scales to the size the
// player was built at instead of a fixed panel.
struct FaceTransform {
    float cosine = 1.0f;
    float sine = 0.0f;
    float translate_x = 0.0f;
    float translate_y = 0.0f;
    float energy_scale = 1.0f;
    float energy_sag = 0.0f;
    float unit_scale = 1.0f;
    float center_x = 0.0f;
    float center_y = 0.0f;
    int width = 1;
    int height = 1;

    void Apply(float ux, float uy, float* px, float* py) const {
        const float dx = (ux - kLogicalCenter) * unit_scale;
        const float dy =
            ((uy - kLogicalCenter) * energy_scale + energy_sag) * unit_scale;
        *px = center_x + dx * cosine - dy * sine + translate_x * unit_scale;
        *py = center_y + dx * sine + dy * cosine + translate_y * unit_scale;
    }
};

// --------------------------------------------------------------- shape emitting

void AddPolygon(geo::ShapeSet& set, const FaceTransform& transform,
                const float* xs, const float* ys, int count) {
    if (set.count >= geo::kMaxShapes || count < 3 ||
        count > geo::kMaxShapePoints) {
        return;
    }
    geo::Shape& shape = set.shapes[set.count++];
    shape.count = static_cast<int16_t>(count);
    int left = transform.width;
    int right = -1;
    int top = transform.height;
    int bottom = -1;
    for (int i = 0; i < count; ++i) {
        shape.x[i] = xs[i];
        shape.y[i] = ys[i];
        left = std::min(left, static_cast<int>(std::floor(xs[i])));
        right = std::max(right, static_cast<int>(std::ceil(xs[i])));
        top = std::min(top, static_cast<int>(std::floor(ys[i])));
        bottom = std::max(bottom, static_cast<int>(std::ceil(ys[i])));
    }
    shape.top = static_cast<int16_t>(std::max(0, top));
    shape.bottom =
        static_cast<int16_t>(std::min(transform.height - 1, bottom));
    set.left = static_cast<int16_t>(std::min<int>(set.left,
                                                  std::max(0, left)));
    set.right = static_cast<int16_t>(
        std::max<int>(set.right, std::min(transform.width - 1, right)));
    set.top = static_cast<int16_t>(std::min<int>(set.top, shape.top));
    set.bottom = static_cast<int16_t>(
        std::max<int>(set.bottom, std::min(transform.height - 1, bottom)));
}

void AddEye(geo::ShapeSet& set, const geo::EyeState& eye,
            const FaceTransform& transform) {
    const art::Eye* source = eye.eye;
    if (source == nullptr) return;
    const int count = std::min<int>(source->count, geo::kMaxShapePoints);
    float xs[geo::kMaxShapePoints];
    float ys[geo::kMaxShapePoints];
    for (int i = 0; i < count; ++i) {
        const float ux = source->center[0] +
                         (source->points[i][0] - source->center[0]) *
                             eye.scale_x + eye.shift_x;
        const float uy = source->center[1] +
                         (source->points[i][1] - source->center[1]) *
                             eye.scale_y + eye.shift_y;
        transform.Apply(ux, uy, &xs[i], &ys[i]);
    }
    AddPolygon(set, transform, xs, ys, count);
}

// The parametric helpers below only back the event symbols (bolt, sparkle,
// orbiting rings); every eye is a traced silhouette.
void AddEllipse(geo::ShapeSet& set, const FaceTransform& transform, float ux,
                float uy, float radius_x, float radius_y) {
    const int segments = static_cast<int>(Clamp(
        kTau * std::max(radius_x, radius_y) * transform.unit_scale / 16.0f,
        8.0f,
        static_cast<float>(geo::kMaxShapePoints)));
    float xs[geo::kMaxShapePoints];
    float ys[geo::kMaxShapePoints];
    for (int i = 0; i < segments; ++i) {
        const float angle = static_cast<float>(i) / segments * kTau;
        transform.Apply(ux + std::cos(angle) * radius_x,
                        uy + std::sin(angle) * radius_y, &xs[i], &ys[i]);
    }
    AddPolygon(set, transform, xs, ys, segments);
}

void AddRing(geo::ShapeSet& set, const FaceTransform& transform, float ux,
             float uy, float radius, float thickness) {
    // Both arcs run the full circle inclusive, so the seam where the outline
    // folds back on itself has zero width and the annulus reads as unbroken.
    constexpr int kSteps = geo::kMaxShapePoints / 2 - 1;
    float xs[geo::kMaxShapePoints];
    float ys[geo::kMaxShapePoints];
    const float inner = std::max(0.4f, radius - thickness);
    for (int i = 0; i <= kSteps; ++i) {
        const float angle = static_cast<float>(i) / kSteps * kTau;
        transform.Apply(ux + std::cos(angle) * radius,
                        uy + std::sin(angle) * radius, &xs[i], &ys[i]);
    }
    for (int i = 0; i <= kSteps; ++i) {
        const float angle = static_cast<float>(kSteps - i) / kSteps * kTau;
        transform.Apply(ux + std::cos(angle) * inner,
                        uy + std::sin(angle) * inner,
                        &xs[kSteps + 1 + i], &ys[kSteps + 1 + i]);
    }
    AddPolygon(set, transform, xs, ys, kSteps * 2 + 2);
}

void AddSegment(geo::ShapeSet& set, const FaceTransform& transform, float ux1,
                float uy1, float ux2, float uy2, float thickness) {
    float x1;
    float y1;
    float x2;
    float y2;
    transform.Apply(ux1, uy1, &x1, &y1);
    transform.Apply(ux2, uy2, &x2, &y2);
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float length = std::max(0.01f, std::sqrt(dx * dx + dy * dy));
    const float half = thickness * transform.unit_scale;
    const float nx = -dy / length * half;
    const float ny = dx / length * half;
    const float xs[4] = {x1 + nx, x2 + nx, x2 - nx, x1 - nx};
    const float ys[4] = {y1 + ny, y2 + ny, y2 - ny, y1 - ny};
    AddPolygon(set, transform, xs, ys, 4);
}

void AddDiamond(geo::ShapeSet& set, const FaceTransform& transform, float ux,
                float uy, float radius) {
    float xs[4];
    float ys[4];
    transform.Apply(ux, uy - radius, &xs[0], &ys[0]);
    transform.Apply(ux + radius, uy, &xs[1], &ys[1]);
    transform.Apply(ux, uy + radius, &xs[2], &ys[2]);
    transform.Apply(ux - radius, uy, &xs[3], &ys[3]);
    AddPolygon(set, transform, xs, ys, 4);
}

// A status glyph is a stack of traced loops whose holes have already been bridged
// into their shell by the generator, so the set-wide union is what paints them.
void AddGlyph(geo::ShapeSet& set, const art::Glyph& glyph,
              const FaceTransform& transform, float scale) {
    float xs[geo::kMaxShapePoints];
    float ys[geo::kMaxShapePoints];
    const int shapes = std::min<int>(glyph.shape_count, geo::kMaxShapes);
    for (int s = 0; s < shapes; ++s) {
        const art::GlyphShape& span = glyph.shapes[s];
        const int count = std::min<int>(span.count, geo::kMaxShapePoints);
        for (int i = 0; i < count; ++i) {
            const art::GlyphPoint& point = art::kGlyphPoints[span.first + i];
            transform.Apply(
                glyph.center[0] + (point.x - glyph.center[0]) * scale,
                glyph.center[1] + (point.y - glyph.center[1]) * scale,
                &xs[i], &ys[i]);
        }
        AddPolygon(set, transform, xs, ys, count);
    }
}

// ---------------------------------------------------------------- anti-aliased fill

void AddSpanCoverage(int16_t* coverage, int x0, int x1, float start,
                     float end) {
    const float a = std::max(start, static_cast<float>(x0));
    const float b = std::min(end, static_cast<float>(x1));
    if (b <= a) return;
    const int first = static_cast<int>(std::floor(a));
    const int last = static_cast<int>(std::ceil(b)) - 1;
    if (first > last) return;
    if (first == last) {
        coverage[first - x0] +=
            static_cast<int16_t>((b - a) * kCoveragePerSubSample + 0.5f);
        return;
    }
    coverage[first - x0] += static_cast<int16_t>(
        (static_cast<float>(first + 1) - a) * kCoveragePerSubSample + 0.5f);
    for (int x = first + 1; x < last; ++x) coverage[x - x0] += kCoveragePerSubSample;
    coverage[last - x0] += static_cast<int16_t>(
        (b - static_cast<float>(last)) * kCoveragePerSubSample + 0.5f);
}

void AccumulateRow(const geo::ShapeSet& set, int y, int x0, int x1,
                   int16_t* coverage) {
    float crossings[kMaxCrossings];
    for (int s = 0; s < set.count; ++s) {
        const geo::Shape& shape = set.shapes[s];
        if (y < shape.top || y > shape.bottom) continue;
        const int count = shape.count;
        for (int sub = 0; sub < kSubSamples; ++sub) {
            const float scan_y =
                static_cast<float>(y) + (sub + 0.5f) / kSubSamples;
            int found = 0;
            for (int i = 0; i < count; ++i) {
                const int previous = i == 0 ? count - 1 : i - 1;
                const float ya = shape.y[previous];
                const float yb = shape.y[i];
                if ((ya <= scan_y && yb > scan_y) ||
                    (yb <= scan_y && ya > scan_y)) {
                    if (found >= kMaxCrossings) break;
                    crossings[found++] =
                        shape.x[previous] +
                        (scan_y - ya) *
                            (shape.x[i] - shape.x[previous]) / (yb - ya);
                }
            }
            for (int i = 1; i < found; ++i) {
                const float value = crossings[i];
                int j = i - 1;
                while (j >= 0 && crossings[j] > value) {
                    crossings[j + 1] = crossings[j];
                    --j;
                }
                crossings[j + 1] = value;
            }
            for (int i = 0; i + 1 < found; i += 2) {
                AddSpanCoverage(coverage, x0, x1, crossings[i],
                                crossings[i + 1]);
            }
        }
    }
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

// Which imported silhouette each action wears. The player only ever animates the
// pose it picks here, so adding an expression is a matter of drawing it in the
// source bitmap sheet and re-running scripts/trace_expression_art.py.
art::Name ArtForAction(expression_spec::Action action) {
    using Action = expression_spec::Action;
    using Name = art::Name;
    switch (action) {
        case Action::Smile:
        case Action::Answering:
        case Action::Complete:
            return Name::happy;
        case Action::Laugh:
            return Name::excited;
        case Action::Yawn:
            return Name::sleepy;
        case Action::Listening:
            return Name::focused;
        case Action::Sleep:
            return Name::blink;
        case Action::Dizzy:
            return Name::disoriented;
        case Action::Connecting:
            return Name::worried;
        case Action::WinkLeft:
            return Name::wink_left;
        case Action::WinkRight:
            return Name::wink_right;
        case Action::BlinkUp:
            return Name::blink_up;
        case Action::BlinkDown:
            return Name::blink_down;
        case Action::LookLeft:
            return Name::look_left;
        case Action::LookRight:
            return Name::look_right;
        case Action::LookUp:
            return Name::look_up;
        case Action::LookDown:
            return Name::look_down;
        case Action::Surprised:
            return Name::surprised;
        case Action::Bored:
            return Name::bored;
        case Action::Sad:
            return Name::sad;
        case Action::Angry:
            return Name::angry;
        case Action::Scared:
            return Name::scared;
        case Action::Despair:
            return Name::despair;
        case Action::Furious:
            return Name::furious;
        case Action::Alert:
            return Name::alert;
        case Action::Idle:
        case Action::Charging:
        case Action::Wake:
        default:
            return Name::normal;
    }
}

// Which whole-panel graphic an action shows. Everything outside this table keeps
// its eye pair. The imported signal, mode and logo graphics have no trigger of
// their own yet, so they stay in the generated tables without an action here.
const art::Glyph* GlyphForAction(expression_spec::Action action) {
    using Action = expression_spec::Action;
    switch (action) {
        case Action::Battery:
            return &art::kGlyphs[art::battery];
        case Action::BatteryFull:
            return &art::kGlyphs[art::battery_full];
        case Action::BatteryLow:
            return &art::kGlyphs[art::battery_low];
        case Action::Warning:
            return &art::kGlyphs[art::warning];
        default:
            return nullptr;
    }
}

}  // namespace

// ------------------------------------------------------------------------ emit

void ExpressionPlayer::Emit(const FrameGeometry& frame,
                            geo::ShapeSet& shapes) const {
    shapes = geo::ShapeSet{};
    FaceTransform transform;
    const float radians = frame.rotation * kPi / 180.0f;
    transform.cosine = std::cos(radians);
    transform.sine = std::sin(radians);
    transform.translate_x = frame.translate_x;
    transform.translate_y = frame.translate_y;
    transform.energy_scale = frame.energy_vertical_scale;
    transform.energy_sag = frame.energy_sag;
    transform.unit_scale = static_cast<float>(surface_width_) /
                           static_cast<float>(art::kLogicalExtent);
    transform.center_x = static_cast<float>(surface_width_) * 0.5f;
    transform.center_y = static_cast<float>(surface_height_) * 0.5f;
    transform.width = surface_width_;
    transform.height = surface_height_;

    if (frame.glyph != nullptr) {
        AddGlyph(shapes, *frame.glyph, transform, frame.glyph_scale);
        return;
    }

    AddEye(shapes, frame.left, transform);
    AddEye(shapes, frame.right, transform);

    const float amount = frame.symbol_amount;
    switch (frame.action) {
        case Action::Charging: {
            // The imported eyes reach out to ~42 units, so the bolt lives in the
            // corner above them instead of overlapping the pair like the old
            // ellipse artwork did.
            const float thickness = 0.45f + SmoothStep(amount) * 0.15f;
            AddSegment(shapes, transform, 46.0f, 9.0f, 43.2f, 13.2f, thickness);
            AddSegment(shapes, transform, 43.2f, 13.2f, 46.8f, 13.8f, thickness);
            AddSegment(shapes, transform, 46.8f, 13.8f, 43.6f, 18.0f, thickness);
            break;
        }
        case Action::Complete: {
            const float sparkle = SegmentPulse(frame.seconds, 0.2f, 1.3f);
            AddDiamond(shapes, transform, 44.0f, 12.5f, 3.0f * sparkle);
            break;
        }
        case Action::Sleep: {
            // The Zzz marker: a Z that climbs away from the right eye and grows
            // while it rises, so the loop reads as one Z drifting off rather
            // than a dot repeating in place.
            const float z_phase =
                std::fmod(std::max(frame.seconds, 0.0f), 2.4f) / 2.4f;
            const float half_width = 1.1f + z_phase * 1.7f;
            const float center_x = 41.5f + z_phase * 3.0f;
            const float center_y = 18.0f - z_phase * 8.0f;
            const float thickness = 0.4f + z_phase * 0.3f;
            AddSegment(shapes, transform, center_x - half_width,
                       center_y - half_width, center_x + half_width,
                       center_y - half_width, thickness);
            AddSegment(shapes, transform, center_x + half_width,
                       center_y - half_width, center_x - half_width,
                       center_y + half_width, thickness);
            AddSegment(shapes, transform, center_x - half_width,
                       center_y + half_width, center_x + half_width,
                       center_y + half_width, thickness);
            break;
        }
        case Action::Dizzy: {
            const float orbit = frame.seconds * kTau * 1.8f;
            const float left_x = frame.left.eye != nullptr
                                     ? frame.left.eye->center[0] + frame.left.shift_x
                                     : 0.0f;
            const float left_y = frame.left.eye != nullptr
                                     ? frame.left.eye->center[1] + frame.left.shift_y
                                     : 0.0f;
            const float right_x = frame.right.eye != nullptr
                                      ? frame.right.eye->center[0] + frame.right.shift_x
                                      : 0.0f;
            const float right_y = frame.right.eye != nullptr
                                      ? frame.right.eye->center[1] + frame.right.shift_y
                                      : 0.0f;
            // The ring has to clear the pose's own eye, which is not a fixed
            // size once the silhouettes come from the imported sheet.
            const float left_radius = frame.left.eye != nullptr
                                          ? std::max(frame.left.eye->radius[0],
                                                     frame.left.eye->radius[1]) *
                                                frame.left.scale_x
                                          : 5.5f;
            const float right_radius = frame.right.eye != nullptr
                                           ? std::max(frame.right.eye->radius[0],
                                                      frame.right.eye->radius[1]) *
                                                 frame.right.scale_x
                                           : 5.5f;
            if (frame.left.eye != nullptr) {
                AddRing(shapes, transform, left_x, left_y,
                        left_radius + 1.6f + amount * 1.3f, 1.2f);
                AddEllipse(shapes, transform, left_x + std::cos(orbit) * 4.2f,
                           left_y + std::sin(orbit) * 4.2f, 1.3f, 1.3f);
            }
            if (frame.right.eye != nullptr) {
                AddRing(shapes, transform, right_x, right_y,
                        right_radius + 1.6f + amount * 1.3f, 1.2f);
                AddEllipse(shapes, transform, right_x - std::cos(orbit) * 4.2f,
                           right_y - std::sin(orbit) * 4.2f, 1.3f, 1.3f);
            }
            break;
        }
        default:
            break;
    }
}

// --------------------------------------------------------------------- rasterise

void ExpressionPlayer::InvalidateRect(int left, int top, int right,
                                      int bottom) {
    if (image_ == nullptr || left > right || top > bottom) return;
    lv_area_t image_area;
    lv_obj_get_coords(image_, &image_area);
    const lv_area_t dirty = {
        .x1 = image_area.x1 + left,
        .y1 = image_area.y1 + top,
        .x2 = image_area.x1 + right,
        .y2 = image_area.y1 + bottom,
    };
    lv_obj_invalidate_area(image_, &dirty);
}

void ExpressionPlayer::Render(float dissolve) {
    if (a8_buffer_ == nullptr || row_coverage_ == nullptr ||
        current_shapes_ == nullptr) {
        return;
    }
    int x0 = surface_width_;
    int y0 = surface_height_;
    int x1 = -1;
    int y1 = -1;
    auto include = [&x0, &y0, &x1, &y1](int left, int top, int right,
                                        int bottom) {
        if (left > right || top > bottom) return;
        x0 = std::min(x0, left);
        y0 = std::min(y0, top);
        x1 = std::max(x1, right);
        y1 = std::max(y1, bottom);
    };
    include(current_shapes_->left, current_shapes_->top, current_shapes_->right,
            current_shapes_->bottom);
    const bool blending = dissolve < 1.0f && previous_shapes_ != nullptr;
    if (blending) {
        include(previous_shapes_->left, previous_shapes_->top,
                previous_shapes_->right, previous_shapes_->bottom);
    }
    // Whatever the previous frame painted has to be re-tested so shapes fade out
    // instead of burning into the surface.
    include(last_left_, last_top_, last_right_, last_bottom_);
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(surface_width_ - 1, x1);
    y1 = std::min(surface_height_ - 1, y1);
    if (x0 > x1 || y0 > y1) return;

    int16_t* incoming = row_coverage_;
    int16_t* outgoing = row_coverage_ + surface_width_;
    const int dissolve_cover =
        static_cast<int>(Clamp(dissolve, 0.0f, 1.0f) * 255.0f);
    int changed_left = surface_width_;
    int changed_top = surface_height_;
    int changed_right = -1;
    int changed_bottom = -1;

    for (int y = y0; y <= y1; ++y) {
        std::memset(incoming + x0, 0,
                    static_cast<size_t>(x1 - x0 + 1) * sizeof(int16_t));
        if (blending) {
            std::memset(outgoing + x0, 0,
                        static_cast<size_t>(x1 - x0 + 1) * sizeof(int16_t));
        }
        AccumulateRow(*current_shapes_, y, x0, x1, incoming);
        if (blending) {
            AccumulateRow(*previous_shapes_, y, x0, x1, outgoing);
        }
        uint8_t* row = a8_buffer_ + static_cast<size_t>(y) * surface_width_;
        for (int x = x0; x <= x1; ++x) {
            int value = std::min<int>(incoming[x - x0], 255);
            if (blending) {
                const int previous = std::min<int>(outgoing[x - x0], 255);
                value = (value * dissolve_cover +
                         previous * (255 - dissolve_cover) + 127) / 255;
            }
            const uint8_t alpha = static_cast<uint8_t>(value);
            if (row[x] == alpha) continue;
            row[x] = alpha;
            changed_left = std::min(changed_left, x);
            changed_right = std::max(changed_right, x);
            changed_top = std::min(changed_top, y);
            changed_bottom = std::max(changed_bottom, y);
        }
    }

    if (changed_right < changed_left) return;
    // The scanned rect already covers the current shapes, the blended-out ones
    // and everything earlier frames touched, so it is what the next frame has to
    // re-test to let stale pixels fade away.
    last_left_ = static_cast<int16_t>(x0);
    last_top_ = static_cast<int16_t>(y0);
    last_right_ = static_cast<int16_t>(x1);
    last_bottom_ = static_cast<int16_t>(y1);
    lv_image_cache_drop(&image_descriptor_);
    InvalidateRect(changed_left, changed_top, changed_right, changed_bottom);
}

// --------------------------------------------------------------------- lifecycle

// pixel_step is a leftover of the old dot-matrix player: the polygon
// rasteriser samples continuously, so a reduced surface needs no coarser grid
// and the argument only exists for its former callers.
ExpressionPlayer::ExpressionPlayer(
    lv_obj_t* parent,
    audio::ListeningAudioFeatureStore* listening_audio_features,
    int surface_width, int surface_height, int pixel_step)
    : parent_(parent),
      surface_width_(std::max(1, surface_width)),
      surface_height_(std::max(1, surface_height)),
      listening_audio_features_(listening_audio_features) {
    if (parent_ == nullptr || !lv_obj_is_valid(parent_)) return;

    lv_obj_add_event_cb(parent_, ParentDeletedCallback, LV_EVENT_DELETE, this);
    const size_t kBufferSize = static_cast<size_t>(surface_width_) *
                               static_cast<size_t>(surface_height_);
    const size_t kScratchSize = sizeof(geo::ShapeSet) * 2 +
                                sizeof(int16_t) * surface_width_ * 2;
    a8_buffer_ = AllocateExpressionBuffer(kBufferSize);
    if (a8_buffer_ == nullptr) {
        ESP_LOGE(kTag, "Unable to allocate %u-byte A8 expression buffer",
                 static_cast<unsigned>(kBufferSize));
        return;
    }
    auto* scratch = static_cast<uint8_t*>(
        heap_caps_malloc(kScratchSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr) {
        scratch = static_cast<uint8_t*>(heap_caps_malloc(
            kScratchSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (scratch == nullptr) {
        ESP_LOGE(kTag, "Unable to allocate expression rasteriser scratch");
        heap_caps_free(a8_buffer_);
        a8_buffer_ = nullptr;
        return;
    }
    shape_sets_ = reinterpret_cast<geo::ShapeSet*>(scratch);
    current_shapes_ = shape_sets_;
    previous_shapes_ = shape_sets_ + 1;
    row_coverage_ =
        reinterpret_cast<int16_t*>(scratch + sizeof(geo::ShapeSet) * 2);
    std::memset(a8_buffer_, 0, kBufferSize);
    RegisterExpressionA8Buffer(a8_buffer_, kBufferSize);

    image_descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_descriptor_.header.cf = LV_COLOR_FORMAT_A8;
    image_descriptor_.header.w = surface_width_;
    image_descriptor_.header.h = surface_height_;
    image_descriptor_.header.stride = surface_width_;
    image_descriptor_.data_size = kBufferSize;
    image_descriptor_.data = a8_buffer_;

    image_ = lv_image_create(parent_);
    lv_image_set_src(image_, &image_descriptor_);
    lv_obj_set_size(image_, surface_width_, surface_height_);
    lv_image_set_inner_align(image_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(image_);
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_SCROLLABLE);
    accent_ = Theme::Get().colors().accent;
    lv_obj_set_style_image_recolor(image_, lv_color_hex(accent_), LV_PART_MAIN);
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

void ExpressionPlayer::PlayComplete() { RequestSpecial(Action::Complete); }

void ExpressionPlayer::PlayDizzy() { RequestSpecial(Action::Dizzy); }

void ExpressionPlayer::PlayBattery() { RequestSpecial(Action::Battery); }

void ExpressionPlayer::PlayBatteryFull() {
    RequestSpecial(Action::BatteryFull);
}

void ExpressionPlayer::PlayBatteryLow() { RequestSpecial(Action::BatteryLow); }

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
            return Action::Connecting;
        case AgentState::Listening:
            return Action::Listening;
        case AgentState::Answering:
            return Action::Answering;
        case AgentState::Error:
            // No duration, so the graphic holds for exactly as long as the fault
            // does and dissolves away when the state moves on.
            return Action::Warning;
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

void ExpressionPlayer::BeginDirectMorph(Action action, float target_seconds,
                                        Energy energy) {
    const uint32_t now = lv_tick_get();
    action_ = action;
    energy_ = energy;
    morph_target_seconds_ = std::max(0.0f, target_seconds);
    if (!has_rendered_ || current_shapes_ == nullptr ||
        previous_shapes_ == nullptr) {
        ActivateAction(action, now, morph_target_seconds_);
        return;
    }
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
        Action::Smile,     Action::Laugh,      Action::Yawn,
        Action::WinkLeft,  Action::WinkRight,  Action::LookLeft,
        Action::LookRight, Action::LookUp,     Action::LookDown,
        Action::Surprised, Action::Bored,      Action::Sad,
        Action::Angry,     Action::Scared,     Action::Despair,
        Action::Furious,   Action::Alert,      Action::BlinkUp,
        Action::BlinkDown,
    };
    RequestAction(kAmbientActions[esp_random() %
                                  (sizeof(kAmbientActions) /
                                   sizeof(kAmbientActions[0]))]);
}

float ExpressionPlayer::ActionEnvelope(Action action, float seconds) const {
    if (action == Action::Idle) return 0.0f;
    if (action == Action::Sleep) return 1.0f;
    const float duration = expression_spec::EnvelopeSeconds(action);
    const bool loop = expression_spec::IsLooping(action) ||
                      (special_expression_held_ && action == held_special_action_);
    if (duration <= 0.0f) return 0.0f;
    const float progress =
        loop ? std::fmod(std::max(seconds, 0.0f), duration) / duration
             : Clamp(seconds / duration, 0.0f, 1.0f);
    const float wave = std::sin(progress * kPi);
    return wave * wave;
}

// ---------------------------------------------------------------------- geometry

ExpressionPlayer::FrameGeometry ExpressionPlayer::BuildFrameGeometry(
    float seconds, float motion_seconds, float intensity_scale) const {
    FrameGeometry frame;
    const bool tracking_expression =
        tracking_active_ && (action_ == Action::Idle ||
                             action_ == Action::Listening ||
                             action_ == Action::Answering);
    const Action rendered = tracking_expression ? Action::Idle : action_;
    frame.action = rendered;
    frame.seconds = seconds;

    const art::Name name = ArtForAction(rendered);
    const art::Expression& pose = art::kExpressions[name];
    frame.left.eye = &pose.left;
    frame.right.eye = &pose.right;

    // A status graphic owns the whole surface, so the eye pair steps out and
    // stays out for as long as the action plays.
    frame.glyph = GlyphForAction(rendered);
    if (frame.glyph != nullptr) {
        frame.left.eye = nullptr;
        frame.right.eye = nullptr;
    }

    // The energy sag exists to droop the eyes, so it is skipped for a glyph,
    // which would only end up squashed.
    if (frame.glyph == nullptr) {
        if (energy_ == Energy::Exhausted) {
            frame.energy_vertical_scale = 0.38f;
            frame.energy_sag = 2.3f;
        } else if (energy_ == Energy::Tired) {
            frame.energy_vertical_scale = 0.68f;
            frame.energy_sag = 1.15f;
        }
    }

    // Slow idle sway: the whole face drifts a couple of units and tilts.
    const float motion_phase =
        std::fmod(std::max(motion_seconds, 0.0f),
                  expression_spec::kMotionDurationSeconds) /
        expression_spec::kMotionDurationSeconds;
    float from_phase = 0.0f;
    float to_phase = 0.42f;
    float from_x = 0.0f;
    float from_y = 0.0f;
    float from_rotation = 0.0f;
    float to_x = 0.2f;
    float to_y = -0.8f;
    float to_rotation = 0.9f;
    if (motion_phase > 0.72f) {
        from_phase = 0.72f;
        to_phase = 1.0f;
        from_x = -0.2f;
        from_y = 0.8f;
        from_rotation = -0.9f;
        to_x = 0.0f;
        to_y = 0.0f;
        to_rotation = 0.0f;
    } else if (motion_phase > 0.42f) {
        from_phase = 0.42f;
        to_phase = 0.72f;
        from_x = 0.2f;
        from_y = -0.8f;
        from_rotation = 0.9f;
        to_x = -0.2f;
        to_y = 0.8f;
        to_rotation = -0.9f;
    }
    const float motion_amount =
        SmoothStep((motion_phase - from_phase) / (to_phase - from_phase));
    frame.translate_x = from_x + (to_x - from_x) * motion_amount;
    frame.translate_y = from_y + (to_y - from_y) * motion_amount;
    frame.rotation = from_rotation + (to_rotation - from_rotation) * motion_amount;
    if (tracking_active_) {
        frame.translate_x = 0.0f;
        frame.translate_y = 0.0f;
        frame.rotation = 0.0f;
    }

    float blink = 0.0f;
    float left_wink = 0.0f;
    float right_wink = 0.0f;
    float idle_look_x = 0.0f;
    if (rendered == Action::Idle) {
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
    const float action_amount =
        ActionEnvelope(rendered, seconds) * intensity_scale;
    frame.symbol_amount = action_amount;
    const float manual_x_scale = tracking_active_ ? 3.25f : 1.25f;
    const float manual_y_scale = tracking_active_ ? 2.75f : 1.2f;
    frame.left.shift_x = frame.right.shift_x =
        look_x_ * manual_x_scale + idle_look_x * 1.25f;
    frame.left.shift_y = frame.right.shift_y = look_y_ * manual_y_scale;

    const float base_scale = 1.0f + breath * 0.02f;
    frame.left.scale_x = frame.left.scale_y = base_scale;
    frame.right.scale_x = frame.right.scale_y = base_scale;

    switch (rendered) {
        case Action::Idle:
            frame.left.scale_y *= 1.0f - std::max(blink, left_wink) * 0.82f;
            frame.right.scale_y *= 1.0f - std::max(blink, right_wink) * 0.82f;
            break;
        case Action::Smile: {
            const float lift = action_amount * 0.8f;
            frame.left.shift_y -= lift;
            frame.right.shift_y -= lift;
            frame.left.scale_y *= 1.0f + action_amount * 0.1f;
            frame.right.scale_y *= 1.0f + action_amount * 0.1f;
            break;
        }
        case Action::Laugh: {
            const float bounce = std::abs(std::sin(seconds * kTau * 2.2f)) *
                                 1.2f * action_amount;
            const float pulse = 1.0f + std::abs(std::sin(seconds * kTau * 1.7f)) *
                                          0.12f * action_amount;
            frame.left.shift_y -= bounce;
            frame.right.shift_y -= bounce;
            frame.left.scale_x *= pulse;
            frame.right.scale_x *= pulse;
            break;
        }
        case Action::Yawn: {
            frame.left.scale_y *= 1.0f + action_amount * 0.45f;
            frame.right.scale_y *= 1.0f + action_amount * 0.45f;
            frame.left.scale_x *= 1.0f + action_amount * 0.1f;
            frame.right.scale_x *= 1.0f + action_amount * 0.1f;
            break;
        }
        case Action::Listening: {
            const float wave = 0.5f + std::sin(seconds * kTau * 1.15f) * 0.5f;
            const float attention =
                std::min(1.0f, listening_vad_ * 0.62f +
                                  listening_onset_ * 0.9f);
            const float elastic =
                listening_onset_ * std::sin(seconds * kTau * 2.2f) * 0.28f;
            const float audio_pulse = listening_activity_ *
                (0.9f + 0.3f * std::sin(seconds * kTau * 1.15f + 0.55f));
            const float squeeze = 1.0f - attention * 0.08f;
            frame.left.shift_x += attention * 0.85f;
            frame.right.shift_x -= attention * 0.85f;
            const float left_pulse =
                (wave - 0.5f) * 0.42f * action_amount + audio_pulse + elastic;
            const float right_pulse =
                (0.5f - wave) * 0.42f * action_amount + audio_pulse - elastic;
            frame.left.scale_x = frame.left.scale_y =
                base_scale * (1.0f + left_pulse * 0.16f) * squeeze;
            frame.right.scale_x = frame.right.scale_y =
                base_scale * (1.0f + right_pulse * 0.16f) * squeeze;
            const float bob = (wave - 0.5f) * 0.75f * action_amount +
                              listening_onset_ *
                                  std::sin(seconds * kTau * 1.55f) * 0.55f;
            frame.left.shift_y -= bob;
            frame.right.shift_y += bob;
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
            frame.left.shift_y -= (kLeftLift[variant] + bounce) * smile_amount;
            frame.right.shift_y -= (kRightLift[variant] + bounce) * smile_amount;
            frame.left.scale_y *= 1.0f + smile_amount * 0.08f;
            frame.right.scale_y *= 1.0f + smile_amount * 0.08f;
            break;
        }
        case Action::Charging: {
            const float pulse = 1.0f + SmoothStep(action_amount) * 0.10f;
            frame.left.scale_x = frame.left.scale_y = base_scale * pulse;
            frame.right.scale_x = frame.right.scale_y = base_scale * pulse;
            break;
        }
        case Action::Complete: {
            const float smile_amount = SmoothStep(action_amount);
            frame.left.shift_y -= smile_amount;
            frame.right.shift_y -= smile_amount;
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
                open = 0.12f + 0.88f * SmoothStep((seconds - 0.5f) / 0.28f);
            }
            frame.left.scale_y *= open;
            frame.right.scale_y *= open;
            break;
        }
        case Action::Sleep: {
            const float drift = std::sin(seconds * kTau * 0.18f) * 0.45f;
            frame.left.shift_y += drift;
            frame.right.shift_y += drift;
            break;
        }
        case Action::Dizzy: {
            const float wobble = std::sin(seconds * kTau * 3.6f) * 1.2f;
            frame.left.shift_x += wobble;
            frame.right.shift_x -= wobble;
            // Keep the eyes small enough for the orbit ring to encircle them.
            frame.left.scale_x = frame.left.scale_y = base_scale * 0.7f;
            frame.right.scale_x = frame.right.scale_y = base_scale * 0.7f;
            break;
        }
        case Action::Connecting: {
            const float sway = std::sin(seconds * kTau / 1.6f) * 0.9f;
            frame.left.shift_x += sway;
            frame.right.shift_x += sway;
            break;
        }
        case Action::Surprised: {
            // The surprised pose is already the widest artwork, so the pop goes
            // upward and slightly narrows the eyes instead of growing the pair.
            const float pop = SmoothStep(action_amount);
            frame.left.scale_x = frame.right.scale_x = base_scale * (1.0f - pop * 0.06f);
            frame.left.scale_y = frame.right.scale_y = base_scale * (1.0f + pop * 0.12f);
            break;
        }
        case Action::Angry: {
            const float lean = action_amount * 0.12f;
            frame.left.scale_y *= 1.0f - lean;
            frame.right.scale_y *= 1.0f - lean;
            break;
        }
        case Action::Scared: {
            // A flinch: the pair snaps wide and drops, then settles back.
            const float flinch = SmoothStep(action_amount);
            frame.left.shift_x -= flinch * 0.9f;
            frame.right.shift_x += flinch * 0.9f;
            frame.translate_y += flinch * 0.7f;
            break;
        }
        case Action::Despair: {
            const float sink = SmoothStep(action_amount);
            frame.left.shift_y += sink * 1.4f;
            frame.right.shift_y += sink * 1.4f;
            frame.left.scale_y = frame.right.scale_y =
                base_scale * (1.0f - sink * 0.18f);
            break;
        }
        case Action::Furious: {
            const float grind = SmoothStep(action_amount);
            frame.left.scale_y *= 1.0f - grind * 0.22f;
            frame.right.scale_y *= 1.0f - grind * 0.22f;
            frame.rotation +=
                std::sin(frame.seconds * kTau * 6.0f) * 0.5f * grind;
            break;
        }
        case Action::Alert: {
            const float snap = SmoothStep(action_amount);
            frame.left.scale_x = frame.right.scale_x =
                base_scale * (1.0f - snap * 0.10f);
            frame.left.scale_y = frame.right.scale_y =
                base_scale * (1.0f + snap * 0.12f);
            break;
        }
        case Action::LookLeft:
        case Action::LookRight: {
            // The imported look-* poses are centred, so the glance itself has to
            // come from the shared eye offset.
            const float glance = 2.0f * SmoothStep(action_amount) *
                                 (rendered == Action::LookLeft ? -1.0f : 1.0f);
            frame.left.shift_x += glance;
            frame.right.shift_x += glance;
            break;
        }
        case Action::LookUp:
        case Action::LookDown: {
            const float glance = 1.4f * SmoothStep(action_amount) *
                                 (rendered == Action::LookUp ? -1.0f : 1.0f);
            frame.left.shift_y += glance;
            frame.right.shift_y += glance;
            break;
        }
        case Action::WinkLeft:
        case Action::WinkRight:
        case Action::Bored:
        case Action::Sad:
        default:
            break;
    }

    if (frame.glyph != nullptr) {
        // The dissolve already handles arriving and leaving, so the graphic only
        // breathes a little while it holds the surface.
        frame.glyph_scale = 1.0f + SmoothStep(action_amount) * 0.08f;
        if (rendered == Action::Warning) {
            // A fault outlasts the animation, so the wobble rides the looping
            // envelope instead of a one-shot progress.
            frame.rotation +=
                std::sin(seconds * kTau * 1.4f) * 1.2f * action_amount;
        }
    }
    KeepEyesApart(frame.left, frame.right);
    return frame;
}

// ------------------------------------------------------------------------ frames

void ExpressionPlayer::UpdateFrame() {
    if (parent_ == nullptr || image_ == nullptr || a8_buffer_ == nullptr ||
        current_shapes_ == nullptr || !lv_obj_is_valid(image_)) {
        return;
    }
    // The A8 buffer only carries alpha, so a theme change costs one style write
    // instead of a re-render. Views are normally rebuilt on a theme change, but a
    // long-lived player (Home, Standby) has to follow along on its own.
    const uint32_t accent = Theme::Get().colors().accent;
    if (accent != accent_) {
        accent_ = accent;
        lv_obj_set_style_image_recolor(image_, lv_color_hex(accent),
                                       LV_PART_MAIN);
        lv_obj_invalidate(image_);
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
        Emit(BuildFrameGeometry(morph_target_seconds_, motion_seconds, 1.0f),
             *current_shapes_);
        Render(SmoothStep(progress));
        has_rendered_ = true;
        if (progress >= 1.0f) {
            morph_active_ = false;
            action_started_ms_ = now;
            action_elapsed_offset_ms_ = static_cast<uint32_t>(
                morph_target_seconds_ * 1000.0f);
            std::swap(current_shapes_, previous_shapes_);
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
            Emit(BuildFrameGeometry(morph_target_seconds_, motion_seconds, 1.0f),
                 *current_shapes_);
            Render(0.0f);
            has_rendered_ = true;
            return;
        }
        seconds = CurrentActionSeconds(now);
    }

    Emit(BuildFrameGeometry(seconds, motion_seconds, 1.0f), *current_shapes_);
    Render(1.0f);
    std::swap(current_shapes_, previous_shapes_);
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
    if (shape_sets_ != nullptr) {
        heap_caps_free(reinterpret_cast<uint8_t*>(shape_sets_));
        shape_sets_ = nullptr;
        current_shapes_ = nullptr;
        previous_shapes_ = nullptr;
        row_coverage_ = nullptr;
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
