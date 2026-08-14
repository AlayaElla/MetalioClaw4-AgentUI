#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace agent_ui::camera {

enum class Lifecycle {
    Load,
    Unload,
    Suspend,
    Resume,
};

enum class ViewMode {
    Camera,
    Review,
    Gallery,
    Viewer,
};

enum class EffectStyle : uint8_t {
    Original,
    Mosaic,
    PrintComic,
    Ascii,
    BlackWhite,
};

struct GalleryPhoto {
    std::string name;
    std::string path;
    std::string date_key;
    std::string date_label;
    std::string time_label;
    std::string size_label;
    int64_t modified = 0;
    std::size_t bytes = 0;

    bool operator==(const GalleryPhoto& other) const {
        return name == other.name && path == other.path &&
               date_key == other.date_key && date_label == other.date_label &&
               time_label == other.time_label && size_label == other.size_label &&
               modified == other.modified && bytes == other.bytes;
    }
};

struct PreviewFrame {
    const uint8_t* data = nullptr;
    std::shared_ptr<uint8_t> owned_data;
    int width = 0;
    int height = 0;
    int buffer_index = -1;
    uint32_t generation = 0;
};

struct DecodedImage {
    std::shared_ptr<uint8_t> pixels;
    std::size_t data_size = 0;
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t stride = 0;
};

enum class IntentType {
    Capture,
    DeleteReview,
    SaveReview,
    OpenGallery,
    GalleryBack,
    OpenViewer,
    ViewerBack,
    DeleteViewer,
    NavigateBack,
    LoadThumbnail,
    PreviewDrawn,
    SetEffect,
};

struct Intent {
    IntentType type = IntentType::NavigateBack;
    std::size_t index = 0;
    int buffer_index = -1;
    EffectStyle effect_style = EffectStyle::Original;
    bool dark_mode = false;

    static Intent Capture() { return {.type = IntentType::Capture}; }
    static Intent DeleteReview() { return {.type = IntentType::DeleteReview}; }
    static Intent SaveReview() { return {.type = IntentType::SaveReview}; }
    static Intent OpenGallery() { return {.type = IntentType::OpenGallery}; }
    static Intent GalleryBack() { return {.type = IntentType::GalleryBack}; }
    static Intent OpenViewer(std::size_t value) {
        return {.type = IntentType::OpenViewer, .index = value};
    }
    static Intent ViewerBack() { return {.type = IntentType::ViewerBack}; }
    static Intent DeleteViewer() { return {.type = IntentType::DeleteViewer}; }
    static Intent NavigateBack() { return {.type = IntentType::NavigateBack}; }
    static Intent LoadThumbnail(std::size_t value) {
        return {.type = IntentType::LoadThumbnail, .index = value};
    }
    static Intent PreviewDrawn(int value) {
        return {.type = IntentType::PreviewDrawn, .buffer_index = value};
    }
    static Intent SetEffect(EffectStyle style, bool dark) {
        return {
            .type = IntentType::SetEffect,
            .effect_style = style,
            .dark_mode = dark,
        };
    }
};

enum class CommandType {
    Start,
    Stop,
    Capture,
    DeleteReview,
    SaveReview,
    OpenGallery,
    OpenViewer,
    DeleteViewer,
    LoadThumbnail,
    PreviewDrawn,
    SetEffect,
};

struct Command {
    CommandType type = CommandType::Stop;
    uint32_t generation = 0;
    std::size_t index = 0;
    int buffer_index = -1;
    EffectStyle effect_style = EffectStyle::Original;
    bool dark_mode = false;
    std::string path;
};

enum class EventType {
    PreviewStarted,
    PreviewStopped,
    PreviewFrameReady,
    ReviewReady,
    SaveStarted,
    SaveFinished,
    GalleryLoaded,
    ThumbnailReady,
    ViewerReady,
    PhotoDeleted,
    Status,
};

enum class StatusCode {
    None,
    SaveSucceeded,
    SaveFailed,
    DeleteFailed,
    CameraStartupFailed,
    BackendMessage,
};

struct Event {
    EventType type = EventType::Status;
    uint32_t generation = 0;
    std::size_t index = 0;
    // The source path travels with decode results so an old index cannot be
    // applied to a reordered gallery after a refresh.
    std::string path;
    bool success = false;
    bool storage_available = true;
    StatusCode status_code = StatusCode::None;
    std::string text;
    std::vector<GalleryPhoto> photos;
    std::shared_ptr<const PreviewFrame> frame;
    // CaptureBackend owns the precomposed RGB565 review surface.  ReviewReady
    // carries this image separately from |frame|, which remains the original
    // sensor frame used by SaveReview.
    std::shared_ptr<const DecodedImage> review_image;
    std::shared_ptr<const DecodedImage> image;

    static Event Status(uint32_t generation, const char* message,
                        StatusCode code = StatusCode::BackendMessage) {
        Event event;
        event.type = EventType::Status;
        event.generation = generation;
        event.status_code = code;
        event.text = message != nullptr ? message : "";
        return event;
    }
};

}  // namespace agent_ui::camera
