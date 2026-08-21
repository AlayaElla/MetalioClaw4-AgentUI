#include "camera_effects.h"

#include <algorithm>

namespace agent_ui::camera::effects::detail {
namespace {

template <typename Pixels>
void Render(Pixels& pixels, const EffectContext& context) {
    constexpr int kBlockSize = 16;
    const Rgb gap = context.dark_mode ? Rgb{17, 20, 17}
                                      : Rgb{241, 240, 236};
    for (int block_y = 0; block_y < context.height; block_y += kBlockSize) {
        for (int block_x = 0; block_x < context.width; block_x += kBlockSize) {
            const int block_w =
                std::min(kBlockSize, context.width - block_x);
            const int block_h =
                std::min(kBlockSize, context.height - block_y);
            const int sample_x = block_x + block_w / 2;
            const int sample_y = block_y + block_h / 2;
            const Rgb sample =
                pixels.Read(sample_y * context.width + sample_x);
            for (int y = 0; y < block_h; ++y) {
                for (int x = 0; x < block_w; ++x) {
                    const bool separator =
                        (x == block_w - 1 && block_x + block_w < context.width) ||
                        (y == block_h - 1 && block_y + block_h < context.height);
                    pixels.Write((block_y + y) * context.width + block_x + x,
                                 separator ? gap : sample);
                }
            }
        }
    }
}

}  // namespace

void ApplyMosaic(Rgb888Pixels& pixels, const EffectContext& context) {
    Render(pixels, context);
}

void ApplyMosaic(Rgb565Pixels& pixels, const EffectContext& context) {
    Render(pixels, context);
}

}  // namespace agent_ui::camera::effects::detail
