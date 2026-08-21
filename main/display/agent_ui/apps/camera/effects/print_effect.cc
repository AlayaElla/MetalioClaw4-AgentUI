#include "camera_effects.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "esp_timer.h"

namespace agent_ui::camera::effects::detail {
namespace {

// A lower cutoff joins nearby weak gradients into a heavier comic outline.
constexpr int kEdgeThreshold = 68;
constexpr int kPlateCount = 4;

enum PlateIndex : size_t {
    Cyan = 0,
    Magenta,
    Yellow,
    Black,
};

struct Plate {
    int step;
    int slope_numerator;
    int slope_denominator;
    int max_radius;
    int coverage_floor;
    Rgb ink;
    int opacity;
};

// Integer slopes approximate 15, 75, 0 and 45 degrees. Each plate uses a
// separate grid so the dots keep a physical CMYK print character.
constexpr std::array<Plate, kPlateCount> kPlates = {{
    {18, 4, 15, 8, 18, {12, 174, 205}, 176},
    {19, 56, 15, 8, 18, {216, 28, 132}, 176},
    {17, 0, 1, 7, 18, {242, 196, 24}, 176},
    {11, 1, 1, 3, 96, {12, 12, 14}, 150},
}};

struct LookupTables {
    std::array<uint8_t, 256> posterize{};
    std::array<uint16_t, 32> luma_r5{};
    std::array<uint16_t, 64> luma_g6{};
    std::array<uint16_t, 32> luma_b5{};
    std::array<uint16_t, 32> posterize_r565{};
    std::array<uint16_t, 64> posterize_g565{};
    std::array<uint16_t, 32> posterize_b565{};
    std::array<std::array<uint8_t, 256>, kPlateCount> radius{};
    std::array<std::array<uint8_t, 256>, kPlateCount> ink_r{};
    std::array<std::array<uint8_t, 256>, kPlateCount> ink_g{};
    std::array<std::array<uint8_t, 256>, kPlateCount> ink_b{};
};

const LookupTables& Tables() {
    static const LookupTables tables = [] {
        LookupTables value;
        for (int input = 0; input < 256; ++input) {
            const int level = (input * 7 + 127) / 255;
            value.posterize[input] =
                static_cast<uint8_t>((level * 255 + 3) / 7);

            for (size_t plate_index = 0; plate_index < kPlates.size();
                 ++plate_index) {
                const Plate& plate = kPlates[plate_index];
                const int base_weight = 255 - plate.opacity;
                value.ink_r[plate_index][input] = static_cast<uint8_t>(
                    (input * base_weight + plate.ink.r * plate.opacity + 127) /
                    255);
                value.ink_g[plate_index][input] = static_cast<uint8_t>(
                    (input * base_weight + plate.ink.g * plate.opacity + 127) /
                    255);
                value.ink_b[plate_index][input] = static_cast<uint8_t>(
                    (input * base_weight + plate.ink.b * plate.opacity + 127) /
                    255);

                if (input <= plate.coverage_floor) continue;
                const int normalized =
                    (input - plate.coverage_floor) * 255 /
                    (255 - plate.coverage_floor);
                int radius = 1;
                while (radius < plate.max_radius &&
                       (radius + 1) * (radius + 1) * 255 <=
                           normalized * plate.max_radius * plate.max_radius) {
                    ++radius;
                }
                value.radius[plate_index][input] =
                    static_cast<uint8_t>(radius);
            }
        }
        for (int code = 0; code < 32; ++code) {
            const int expanded = code * 255 / 31;
            value.luma_r5[code] = static_cast<uint16_t>(expanded * 77);
            value.luma_b5[code] = static_cast<uint16_t>(expanded * 29);
            value.posterize_r565[code] = static_cast<uint16_t>(
                (value.posterize[expanded] & 0xF8) << 8);
            value.posterize_b565[code] = static_cast<uint16_t>(
                value.posterize[expanded] >> 3);
        }
        for (int code = 0; code < 64; ++code) {
            const int expanded = code * 255 / 63;
            value.luma_g6[code] = static_cast<uint16_t>(expanded * 150);
            value.posterize_g565[code] = static_cast<uint16_t>(
                (value.posterize[expanded] & 0xFC) << 3);
        }
        return value;
    }();
    return tables;
}

int Coverage(const Rgb& sample, size_t plate_index) {
    const int maximum = std::max({sample.r, sample.g, sample.b});
    switch (plate_index) {
        case Cyan:
            return maximum - sample.r;
        case Magenta:
            return maximum - sample.g;
        case Yellow:
            return maximum - sample.b;
        case Black:
            return 255 - maximum;
    }
    return 0;
}

Rgb Posterize(const Rgb& sample, const LookupTables& tables) {
    return {
        tables.posterize[static_cast<uint8_t>(sample.r)],
        tables.posterize[static_cast<uint8_t>(sample.g)],
        tables.posterize[static_cast<uint8_t>(sample.b)],
    };
}

Rgb BlendInk(const Rgb& base, size_t plate_index,
             const LookupTables& tables) {
    return {
        tables.ink_r[plate_index][static_cast<uint8_t>(base.r)],
        tables.ink_g[plate_index][static_cast<uint8_t>(base.g)],
        tables.ink_b[plate_index][static_cast<uint8_t>(base.b)],
    };
}

int FirstCenter(int minimum, int step) {
    const int first = step / 2;
    if (minimum <= first) return first;
    return first + ((minimum - first + step - 1) / step) * step;
}

template <typename Visitor>
size_t ForEachDot(const EffectContext& context, Visitor&& visitor) {
    size_t dot_index = 0;
    for (size_t plate_index = 0; plate_index < kPlates.size(); ++plate_index) {
        const Plate& plate = kPlates[plate_index];
        const int first_y = FirstCenter(
            context.origin_y - plate.max_radius, plate.step);
        for (int global_y = first_y;
             global_y < context.origin_y + context.height + plate.max_radius;
             global_y += plate.step) {
            const int center_y = global_y - context.origin_y;
            const int sample_y = std::clamp(center_y, 0, context.height - 1);
            const int offset =
                ((global_y * plate.slope_numerator +
                  plate.slope_denominator / 2) /
                 plate.slope_denominator) %
                plate.step;
            for (int center_x = plate.step / 2 + offset;
                 center_x < context.width + plate.max_radius;
                 center_x += plate.step) {
                visitor(dot_index, plate_index, center_x, center_y, sample_y);
                ++dot_index;
            }
        }
    }
    return dot_index;
}

template <typename Pixels>
size_t CaptureRadii(Pixels& pixels, const EffectContext& context,
                    const LookupTables& tables, size_t capacity) {
    if (context.scratch == nullptr || capacity == 0) return 0;
    const size_t count = ForEachDot(
        context,
        [&](size_t dot_index, size_t plate_index, int center_x, int,
            int sample_y) {
            if (dot_index >= capacity) return;
            const int sample_x = std::clamp(center_x, 0, context.width - 1);
            const Rgb sample =
                pixels.Read(sample_y * context.width + sample_x);
            context.scratch[dot_index] =
                tables.radius[plate_index][Coverage(sample, plate_index)];
        });
    return count <= capacity ? count : 0;
}

void FillLumaRow(const Rgb888Pixels& pixels, int width, int y,
                 uint8_t* destination, const LookupTables&) {
    const uint8_t* source =
        pixels.data + static_cast<size_t>(y) * width * 3;
    for (int x = 0; x < width; ++x, source += 3) {
        destination[x] = static_cast<uint8_t>(
            (source[2] * 77 + source[1] * 150 + source[0] * 29) >> 8);
    }
}

void FillLumaRow(const Rgb565Pixels& pixels, int width, int y,
                 uint8_t* destination, const LookupTables& tables) {
    const uint16_t* source = pixels.data + static_cast<size_t>(y) * width;
    for (int x = 0; x < width; ++x) {
        const uint16_t value = source[x];
        destination[x] = static_cast<uint8_t>(
            (tables.luma_r5[(value >> 11) & 0x1F] +
             tables.luma_g6[(value >> 5) & 0x3F] +
             tables.luma_b5[value & 0x1F]) >>
            8);
    }
}

inline bool IsEdge(int luma, int right_luma, int down_luma) {
    const int edge = std::abs(luma - right_luma) +
                     std::abs(luma - down_luma);
    return edge > kEdgeThreshold;
}

inline void WriteBaseRgb888(uint8_t* pixel, int luma, int right_luma,
                            int down_luma, const LookupTables& tables) {
    if (IsEdge(luma, right_luma, down_luma)) {
        pixel[0] = 6;
        pixel[1] = 7;
        pixel[2] = 8;
        return;
    }
    pixel[0] = tables.posterize[pixel[0]];
    pixel[1] = tables.posterize[pixel[1]];
    pixel[2] = tables.posterize[pixel[2]];
}

inline void WriteBaseRgb565(uint16_t* pixel, int luma, int right_luma,
                            int down_luma, const LookupTables& tables) {
    if (IsEdge(luma, right_luma, down_luma)) {
        *pixel = 0x0820;
        return;
    }
    const uint16_t value = *pixel;
    *pixel = static_cast<uint16_t>(
        tables.posterize_r565[(value >> 11) & 0x1F] |
        tables.posterize_g565[(value >> 5) & 0x3F] |
        tables.posterize_b565[value & 0x1F]);
}

template <typename Pixels>
void RenderBaseFallback(Pixels& pixels, const EffectContext& context,
                        const LookupTables& tables) {
    for (int y = 0; y < context.height; ++y) {
        const int down_y = std::min(y + 1, context.height - 1);
        for (int x = 0; x < context.width; ++x) {
            const int index = y * context.width + x;
            const Rgb sample = pixels.Read(index);
            const int right_x = std::min(x + 1, context.width - 1);
            pixels.Write(
                index,
                IsEdge(Luma(sample),
                       Luma(pixels.Read(y * context.width + right_x)),
                       Luma(pixels.Read(down_y * context.width + x)))
                    ? Rgb{8, 7, 6}
                    : Posterize(sample, tables));
        }
    }
}

void RenderBase(Rgb888Pixels& pixels, const EffectContext& context,
                const LookupTables& tables, uint8_t* luma_rows) {
    if (luma_rows == nullptr) {
        RenderBaseFallback(pixels, context, tables);
        return;
    }
    uint8_t* current_luma = luma_rows;
    uint8_t* down_luma = luma_rows + context.width;
    FillLumaRow(pixels, context.width, 0, current_luma, tables);
    FillLumaRow(pixels, context.width,
                std::min(1, context.height - 1), down_luma, tables);

    for (int y = 0; y < context.height; ++y) {
        uint8_t* pixel =
            pixels.data + static_cast<size_t>(y) * context.width * 3;
        const uint8_t* const below =
            y + 1 < context.height ? down_luma : current_luma;
        const int last_x = context.width - 1;
        for (int x = 0; x < last_x; ++x, pixel += 3) {
            WriteBaseRgb888(pixel, current_luma[x], current_luma[x + 1],
                            below[x], tables);
        }
        WriteBaseRgb888(pixel, current_luma[last_x], current_luma[last_x],
                        below[last_x], tables);
        if (y + 1 < context.height) {
            std::swap(current_luma, down_luma);
            if (y + 2 < context.height) {
                FillLumaRow(pixels, context.width, y + 2, down_luma, tables);
            }
        }
    }
}

void RenderBase(Rgb565Pixels& pixels, const EffectContext& context,
                const LookupTables& tables, uint8_t* luma_rows) {
    if (luma_rows == nullptr) {
        RenderBaseFallback(pixels, context, tables);
        return;
    }
    uint8_t* current_luma = luma_rows;
    uint8_t* down_luma = luma_rows + context.width;
    FillLumaRow(pixels, context.width, 0, current_luma, tables);
    FillLumaRow(pixels, context.width,
                std::min(1, context.height - 1), down_luma, tables);

    for (int y = 0; y < context.height; ++y) {
        uint16_t* pixel = pixels.data + static_cast<size_t>(y) * context.width;
        const uint8_t* const below =
            y + 1 < context.height ? down_luma : current_luma;
        const int last_x = context.width - 1;
        for (int x = 0; x < last_x; ++x) {
            WriteBaseRgb565(pixel + x, current_luma[x], current_luma[x + 1],
                            below[x], tables);
        }
        WriteBaseRgb565(pixel + last_x, current_luma[last_x],
                        current_luma[last_x], below[last_x], tables);
        if (y + 1 < context.height) {
            std::swap(current_luma, down_luma);
            if (y + 2 < context.height) {
                FillLumaRow(pixels, context.width, y + 2, down_luma, tables);
            }
        }
    }
}

template <typename Pixels>
void DrawDot(Pixels& pixels, const EffectContext& context,
             const LookupTables& tables, size_t plate_index, int center_x,
             int center_y, int radius) {
    const int radius_squared = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        const int pixel_y = center_y + y;
        if (pixel_y < 0 || pixel_y >= context.height) continue;
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y > radius_squared) continue;
            const int pixel_x = center_x + x;
            if (pixel_x < 0 || pixel_x >= context.width) continue;
            const int index = pixel_y * context.width + pixel_x;
            pixels.Write(index,
                         BlendInk(pixels.Read(index), plate_index, tables));
        }
    }
}

template <typename Pixels>
void RenderDots(Pixels& pixels, const EffectContext& context,
                const LookupTables& tables, size_t dot_count) {
    if (dot_count == 0) return;
    ForEachDot(context,
               [&](size_t dot_index, size_t plate_index, int center_x,
                   int center_y, int) {
                   if (dot_index >= dot_count) return;
                   const int radius = context.scratch[dot_index];
                   if (radius == 0) return;
                   DrawDot(pixels, context, tables, plate_index, center_x,
                           center_y, radius);
               });
}

template <typename Pixels>
void Render(Pixels& pixels, const EffectContext& context) {
    const LookupTables& tables = Tables();
    EffectStageTiming* timing = context.stage_timing;
    const size_t luma_bytes = static_cast<size_t>(context.width) * 2;
    const bool has_luma_cache =
        context.scratch != nullptr && context.height > 0 && context.width > 0 &&
        context.scratch_size >= luma_bytes;
    uint8_t* const luma_rows =
        has_luma_cache ? context.scratch + context.scratch_size - luma_bytes
                       : nullptr;
    const size_t radius_capacity =
        has_luma_cache ? context.scratch_size - luma_bytes
                       : context.scratch_size;

    int64_t started_us = esp_timer_get_time();
    const size_t dot_count =
        CaptureRadii(pixels, context, tables, radius_capacity);
    if (timing != nullptr) {
        timing->capture_radii_us = static_cast<uint32_t>(
            esp_timer_get_time() - started_us);
    }

    started_us = esp_timer_get_time();
    RenderBase(pixels, context, tables, luma_rows);
    if (timing != nullptr) {
        timing->render_base_us = static_cast<uint32_t>(
            esp_timer_get_time() - started_us);
    }

    started_us = esp_timer_get_time();
    RenderDots(pixels, context, tables, dot_count);
    if (timing != nullptr) {
        timing->render_dots_us = static_cast<uint32_t>(
            esp_timer_get_time() - started_us);
    }
}

}  // namespace

void ApplyPrint(Rgb888Pixels& pixels, const EffectContext& context) {
    Render(pixels, context);
}

void ApplyPrint(Rgb565Pixels& pixels, const EffectContext& context) {
    Render(pixels, context);
}

}  // namespace agent_ui::camera::effects::detail
