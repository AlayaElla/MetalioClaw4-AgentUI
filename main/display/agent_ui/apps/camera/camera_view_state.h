#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "camera_contract.h"

namespace agent_ui::camera {

struct ViewState {
    ViewMode mode = ViewMode::Camera;
    bool mounted = false;
    bool preview_running = false;
    bool frozen = false;
    bool review_ready = false;
    bool saving = false;
    bool gallery_loading = false;
    bool gallery_available = false;
    bool viewer_loading = false;
    EffectStyle effect_style = EffectStyle::Original;
    bool effect_dark_mode = false;
    uint32_t generation = 0;
    StatusCode status_code = StatusCode::None;
    std::string status;
    std::vector<GalleryPhoto> gallery;
    std::vector<std::shared_ptr<const DecodedImage>> thumbnails;
    std::vector<bool> thumbnail_failures;
    std::size_t viewer_index = 0;
    std::shared_ptr<const DecodedImage> viewer_image;
    std::shared_ptr<const PreviewFrame> preview_frame;
    std::shared_ptr<const DecodedImage> review_image;

    bool operator==(const ViewState& other) const {
        return mode == other.mode && mounted == other.mounted &&
               preview_running == other.preview_running && frozen == other.frozen &&
               review_ready == other.review_ready && saving == other.saving &&
               gallery_loading == other.gallery_loading &&
               gallery_available == other.gallery_available &&
               viewer_loading == other.viewer_loading &&
               effect_style == other.effect_style &&
               effect_dark_mode == other.effect_dark_mode &&
               generation == other.generation &&
               status_code == other.status_code &&
               status == other.status && gallery == other.gallery &&
               thumbnails == other.thumbnails &&
               thumbnail_failures == other.thumbnail_failures &&
               viewer_index == other.viewer_index &&
               viewer_image == other.viewer_image && preview_frame == other.preview_frame &&
               review_image == other.review_image;
    }

    bool operator!=(const ViewState& other) const { return !(*this == other); }
};

}  // namespace agent_ui::camera
