#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace agent_ui::camera::effects {

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

inline uint16_t PackRgb565(int r, int g, int b) {
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    return static_cast<uint16_t>(((r & 0xF8) << 8) |
                                 ((g & 0xFC) << 3) | (b >> 3));
}

inline int Luma(const Rgb& value) {
    return (value.r * 77 + value.g * 150 + value.b * 29) >> 8;
}

struct Rgb888Pixels {
    uint8_t* data = nullptr;

    Rgb Read(int index) const {
        const uint8_t* pixel = data + static_cast<size_t>(index) * 3;
        return {pixel[2], pixel[1], pixel[0]};
    }

    void Write(int index, const Rgb& value) {
        uint8_t* pixel = data + static_cast<size_t>(index) * 3;
        pixel[0] = static_cast<uint8_t>(std::clamp(value.b, 0, 255));
        pixel[1] = static_cast<uint8_t>(std::clamp(value.g, 0, 255));
        pixel[2] = static_cast<uint8_t>(std::clamp(value.r, 0, 255));
    }
};

struct Rgb565Pixels {
    uint16_t* data = nullptr;

    Rgb Read(int index) const {
        const uint16_t value = data[index];
        return {
            ((value >> 11) & 0x1F) * 255 / 31,
            ((value >> 5) & 0x3F) * 255 / 63,
            (value & 0x1F) * 255 / 31,
        };
    }

    void Write(int index, const Rgb& value) {
        data[index] = PackRgb565(value.r, value.g, value.b);
    }
};

}  // namespace agent_ui::camera::effects
