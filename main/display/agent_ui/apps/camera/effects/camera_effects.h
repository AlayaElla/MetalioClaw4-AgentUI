#pragma once

#include <cstddef>
#include <cstdint>

#include "camera_contract.h"
#include "effect_types.h"

namespace agent_ui::camera::effects {

constexpr size_t kEffectScratchBytes = 12000;
constexpr int64_t kAnimationFrameIntervalUs = 90000;

struct EffectStageTiming {
    uint32_t capture_radii_us = 0;
    uint32_t render_base_us = 0;
    uint32_t render_dots_us = 0;
};

struct EffectContext {
    int width = 0;
    int height = 0;
    int origin_y = 0;
    bool dark_mode = false;
    uint32_t animation_frame = 0;
    uint8_t* scratch = nullptr;
    size_t scratch_size = 0;
    EffectStageTiming* stage_timing = nullptr;
};

void ApplyEffect(Rgb888Pixels& pixels, EffectStyle style,
                 const EffectContext& context);
void ApplyEffect(Rgb565Pixels& pixels, EffectStyle style,
                 const EffectContext& context);
const char* EffectName(EffectStyle style);

namespace detail {

#define DECLARE_CAMERA_EFFECT(name)                                      \
    void name(Rgb888Pixels& pixels, const EffectContext& context);       \
    void name(Rgb565Pixels& pixels, const EffectContext& context)

DECLARE_CAMERA_EFFECT(ApplyOriginal);
DECLARE_CAMERA_EFFECT(ApplyMosaic);
DECLARE_CAMERA_EFFECT(ApplyPrint);
DECLARE_CAMERA_EFFECT(ApplyAscii);
DECLARE_CAMERA_EFFECT(ApplyBlackWhite);

#undef DECLARE_CAMERA_EFFECT

}  // namespace detail
}  // namespace agent_ui::camera::effects
