#include "camera_image_codec.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "jpg/image_to_jpeg.h"
#include "jpg/jpeg_to_image.h"

namespace agent_ui::camera::codec {
namespace {

constexpr uint32_t kRgb565Format = 0x50424752u;

struct JpegOutput {
    std::vector<uint8_t>* bytes = nullptr;
};

std::size_t AppendJpeg(void* arg, std::size_t, const void* data, std::size_t size) {
    auto* output = static_cast<JpegOutput*>(arg);
    if (output == nullptr || output->bytes == nullptr || data == nullptr) return 0;
    const auto* source = static_cast<const uint8_t*>(data);
    output->bytes->insert(output->bytes->end(), source, source + size);
    return size;
}

}  // namespace

bool EncodeRgb565(const uint8_t* pixels, std::size_t size, uint16_t width,
                  uint16_t height, uint8_t quality,
                  std::vector<uint8_t>& jpeg) {
    jpeg.clear();
    if (pixels == nullptr || size == 0 || width == 0 || height == 0) return false;
    JpegOutput output{.bytes = &jpeg};
    if (!image_to_jpeg_software_cb(
            const_cast<uint8_t*>(pixels), size, width, height,
            static_cast<v4l2_pix_fmt_t>(kRgb565Format), quality, AppendJpeg,
            &output)) {
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
