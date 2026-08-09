#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "camera_contract.h"

namespace agent_ui::camera {

class ImageDecoder {
public:
    using EventSink = std::function<void(const Event&)>;

    ImageDecoder();
    ~ImageDecoder();

    void SetEventSink(EventSink sink);
    void Cancel();
    void DecodeThumbnail(uint32_t generation, std::size_t index,
                         const std::string& path);
    void DecodeViewer(uint32_t generation, std::size_t index,
                      const std::string& path);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace agent_ui::camera
