#include "camera_effects.h"

#include <algorithm>
#include <array>

namespace agent_ui::camera::effects::detail {
namespace {

constexpr int kCellWidth = 7;
constexpr int kCellHeight = 10;

constexpr std::array<std::array<uint8_t, 5>, 16> kGlyphs = {{
    {{0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {{0x00, 0x42, 0x7F, 0x40, 0x00}},
    {{0x42, 0x61, 0x51, 0x49, 0x46}},
    {{0x21, 0x41, 0x45, 0x4B, 0x31}},
    {{0x18, 0x14, 0x12, 0x7F, 0x10}},
    {{0x27, 0x45, 0x45, 0x45, 0x39}},
    {{0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {{0x01, 0x71, 0x09, 0x05, 0x03}},
    {{0x36, 0x49, 0x49, 0x49, 0x36}},
    {{0x06, 0x49, 0x49, 0x29, 0x1E}},
    {{0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {{0x7F, 0x49, 0x49, 0x49, 0x36}},
    {{0x3E, 0x41, 0x41, 0x41, 0x22}},
    {{0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {{0x7F, 0x49, 0x49, 0x49, 0x41}},
    {{0x7F, 0x09, 0x09, 0x09, 0x01}},
}};

uint32_t Noise(int column, int row, uint32_t frame, uint32_t salt) {
    uint32_t value = static_cast<uint32_t>(column + 1) * 374761393U;
    value ^= static_cast<uint32_t>(row + 1) * 668265263U;
    value ^= (frame + 1U) * 2246822519U;
    value ^= (salt + 1U) * 3266489917U;
    value = (value ^ (value >> 13U)) * 1274126177U;
    return value ^ (value >> 16U);
}

template <typename Pixels>
void Render(Pixels& pixels, const EffectContext& context) {
    const int columns = (context.width + kCellWidth - 1) / kCellWidth;
    const int rows = (context.height + kCellHeight - 1) / kCellHeight + 2;
    const size_t luma_bytes = static_cast<size_t>(columns) * rows;
    if (context.scratch == nullptr || luma_bytes > context.scratch_size) {
        ApplyBlackWhite(pixels, context);
        return;
    }

    for (int row = 0; row < rows; ++row) {
        const int sample_y = std::clamp(
            row * kCellHeight + kCellHeight / 2, 0, context.height - 1);
        for (int column = 0; column < columns; ++column) {
            const int sample_x = std::clamp(
                column * kCellWidth + kCellWidth / 2, 0, context.width - 1);
            context.scratch[row * columns + column] = static_cast<uint8_t>(
                Luma(pixels.Read(sample_y * context.width + sample_x)));
        }
    }

    for (int index = 0; index < context.width * context.height; ++index) {
        pixels.Write(index, {0, 0, 0});
    }

    for (int column = 0; column < columns; ++column) {
        if (Noise(column, 0, 0, 3) % 29U == 0U) continue;
        const int speed = 1 + static_cast<int>(Noise(column, 0, 0, 5) % 3U);
        const int offset = static_cast<int>(
            (Noise(column, 0, 0, 7) % kCellHeight +
             context.animation_frame * static_cast<uint32_t>(speed * 2)) %
            kCellHeight);
        const int head_row = static_cast<int>(
            (context.animation_frame * static_cast<uint32_t>(speed) +
             column * 7U) % rows);
        for (int row = -1; row < rows; ++row) {
            if (Noise(column, row, context.animation_frame / 3U, 11) % 23U ==
                0U) {
                continue;
            }
            const int y = row * kCellHeight + offset;
            const int sample_row = std::clamp(row, 0, rows - 1);
            const int brightness = context.scratch[sample_row * columns + column];
            const bool head = ((row % rows) + rows) % rows == head_row;
            const Rgb color = head
                                  ? Rgb{183, 255, 208}
                                  : Rgb{0, 32 + brightness * 213 / 255,
                                        8 + brightness * 54 / 255};
            const auto& glyph =
                kGlyphs[Noise(column, row, context.animation_frame, 13) %
                        kGlyphs.size()];
            for (int glyph_x = 0; glyph_x < 5; ++glyph_x) {
                for (int glyph_y = 0; glyph_y < 7; ++glyph_y) {
                    if ((glyph[glyph_x] & (1U << glyph_y)) == 0U) continue;
                    const int pixel_x = column * kCellWidth + glyph_x + 1;
                    const int pixel_y = y + glyph_y + 1;
                    if (pixel_x < 0 || pixel_x >= context.width ||
                        pixel_y < 0 || pixel_y >= context.height) {
                        continue;
                    }
                    pixels.Write(pixel_y * context.width + pixel_x, color);
                }
            }
        }
    }
}

}  // namespace

void ApplyAscii(Rgb888Pixels& pixels, const EffectContext& context) {
    Render(pixels, context);
}

void ApplyAscii(Rgb565Pixels& pixels, const EffectContext& context) {
    Render(pixels, context);
}

}  // namespace agent_ui::camera::effects::detail
