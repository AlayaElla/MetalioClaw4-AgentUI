#include "camera_effects.h"

namespace agent_ui::camera::effects {
namespace {

template <typename Pixels>
void Dispatch(Pixels& pixels, EffectStyle style, const EffectContext& context) {
    switch (style) {
        case EffectStyle::Original:
            detail::ApplyOriginal(pixels, context);
            break;
        case EffectStyle::Mosaic:
            detail::ApplyMosaic(pixels, context);
            break;
        case EffectStyle::PrintComic:
            detail::ApplyPrint(pixels, context);
            break;
        case EffectStyle::Ascii:
            detail::ApplyAscii(pixels, context);
            break;
        case EffectStyle::BlackWhite:
            detail::ApplyBlackWhite(pixels, context);
            break;
    }
}

}  // namespace

void ApplyEffect(Rgb888Pixels& pixels, EffectStyle style,
                 const EffectContext& context) {
    Dispatch(pixels, style, context);
}

void ApplyEffect(Rgb565Pixels& pixels, EffectStyle style,
                 const EffectContext& context) {
    Dispatch(pixels, style, context);
}

const char* EffectName(EffectStyle style) {
    switch (style) {
        case EffectStyle::Original:
            return "original";
        case EffectStyle::Mosaic:
            return "mosaic";
        case EffectStyle::PrintComic:
            return "print";
        case EffectStyle::Ascii:
            return "ascii";
        case EffectStyle::BlackWhite:
            return "black_white";
    }
    return "unknown";
}

}  // namespace agent_ui::camera::effects
