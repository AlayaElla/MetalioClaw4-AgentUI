#include "camera_image_codec.h"

#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "jpg/image_to_jpeg.h"
#include "jpg/jpeg_to_image.h"

namespace agent_ui::camera::codec {
namespace {

constexpr uint32_t kRgb565Format = 0x50424752u;

struct JpegOutput {
    std::vector<uint8_t>* bytes = nullptr;
    bool failed = false;
};

std::size_t AppendJpeg(void* arg, std::size_t, const void* data, std::size_t size) {
    auto* output = static_cast<JpegOutput*>(arg);
    if (output == nullptr || output->bytes == nullptr || data == nullptr || output->failed) return 0;
    const auto* source = static_cast<const uint8_t*>(data);
    try {
        output->bytes->insert(output->bytes->end(), source, source + size);
    } catch (const std::bad_alloc&) {
        // Keep exceptions inside the callback so the encoder can free its
        // conversion and output buffers before reporting failure.
        output->failed = true;
        return 0;
    }
    return size;
}

}  // namespace

bool EncodeRgb565(const uint8_t* pixels, std::size_t size, uint16_t width,
                  uint16_t height, uint8_t quality,
                  std::vector<uint8_t>& jpeg) {
    jpeg.clear();
    if (pixels == nullptr || width == 0 || height == 0 ||
        size < static_cast<std::size_t>(width) * height * sizeof(uint16_t)) return false;
    JpegOutput output{.bytes = &jpeg};
    if (!image_to_jpeg_software_cb(
            const_cast<uint8_t*>(pixels), size, width, height,
            static_cast<v4l2_pix_fmt_t>(kRgb565Format), quality, AppendJpeg,
            &output) || output.failed) {
        jpeg.clear();
        return false;
    }
    return !jpeg.empty();
}

bool DecodeJpeg(const uint8_t* jpeg, std::size_t size, DecodedBuffer& output) {
    output = {};
    if (jpeg == nullptr || size == 0) return false;
    if (jpeg_to_image_software(jpeg, size, &output.data, &output.size,
                               &output.width, &output.height,
                               &output.stride) != ESP_OK) {
        output = {};
        return false;
    }
    if (output.data == nullptr || output.size == 0 || output.width == 0 ||
        output.height == 0 || output.stride < output.width * 2) {
        ReleaseDecoded(output);
        return false;
    }
    return true;
}

void ReleaseDecoded(DecodedBuffer& output) {
    if (output.data != nullptr) heap_caps_free(output.data);
    output = {};
}

}  // namespace agent_ui::camera::codec
