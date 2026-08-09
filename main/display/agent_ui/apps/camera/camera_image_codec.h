#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace agent_ui::camera::codec {

struct DecodedBuffer {
    uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t stride = 0;
};

bool EncodeRgb565(const uint8_t* pixels, std::size_t size, uint16_t width,
                  uint16_t height, uint8_t quality,
                  std::vector<uint8_t>& jpeg);

bool DecodeJpeg(const uint8_t* jpeg, std::size_t size, DecodedBuffer& output);

void ReleaseDecoded(DecodedBuffer& output);

}  // namespace agent_ui::camera::codec
