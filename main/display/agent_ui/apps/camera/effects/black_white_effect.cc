#include "camera_effects.h"

#include <algorithm>

namespace agent_ui::camera::effects::detail {
namespace {

template <typename Pixels>
void Render(Pixels& pixels, const EffectContext& context) {
    const int pixel_count = context.width * context.height;
    for (int index = 0; index < pixel_count; ++index) {
        int gray = Luma(pixels.Read(index));
        gray = std::clamp(((gray - 128) * 44) / 32 + 132, 0, 255);
        pixels.Write(index, {gray, gray, gray});
    }
}

}  // namespace

void ApplyBlackWhite(Rgb888Pixels& pixels, const EffectContext& context) {
    Render(pixels, context);
}

void ApplyBlackWhite(Rgb565Pixels& pixels, const EffectContext& context) {
    Render(pixels, context);
}

}  // namespace agent_ui::camera::effects::detail
