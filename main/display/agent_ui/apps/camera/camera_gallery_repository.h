#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "camera_contract.h"

namespace agent_ui::camera {

class GalleryRepository {
public:
    // Gallery intentionally exposes only the newest entries to bound the
    // number of LVGL cards and decoded PSRAM thumbnails.
    static constexpr std::size_t kMaxItems = 48;
    static constexpr int kSaveWidth = 360;
    static constexpr int kSaveHeight = 236;
    static constexpr int kJpegQuality = 55;

    std::vector<GalleryPhoto> List() const;
    bool Delete(const std::string& path) const;
    bool WriteJpeg(const std::vector<uint8_t>& jpeg, std::string* path) const;
};

}  // namespace agent_ui::camera
