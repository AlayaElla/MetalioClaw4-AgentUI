#include "camera_controller.h"

#include <utility>

namespace agent_ui::camera {

void Controller::Activate(StateSink state_sink, CommandSink command_sink) {
    state_sink_ = std::move(state_sink);
    command_sink_ = std::move(command_sink);
    PublishState();
}

void Controller::Deactivate() {
    state_sink_ = nullptr;
    command_sink_ = nullptr;
}

void Controller::Dispatch(Command command) const {
    if (command_sink_) command_sink_(command);
}

void Controller::PublishState() const {
    if (state_sink_) state_sink_(state_);
}

void Controller::ResetForUnload() {
    const uint32_t next_generation = state_.generation + 1;
    state_ = {};
    state_.generation = next_generation;
}

void Controller::HandleLifecycle(AppLifecycleEvent event) {
    switch (event) {
        case AppLifecycleEvent::Load:
            ++state_.generation;
            state_.mounted = true;
            state_.mode = ViewMode::Camera;
            state_.preview_running = false;
            state_.frozen = false;
            state_.review_ready = false;
            state_.review_image.reset();
            state_.saving = false;
            state_.gallery_loading = false;
            state_.gallery_available = false;
            state_.viewer_loading = false;
            Dispatch({.type = CommandType::Start, .generation = state_.generation});
            break;
        case AppLifecycleEvent::Resume:
            state_.mounted = true;
            state_.preview_running = false;
            state_.preview_frame.reset();
            Dispatch({.type = CommandType::Start, .generation = state_.generation});
            break;
        case AppLifecycleEvent::Suspend:
            state_.preview_running = false;
            Dispatch({.type = CommandType::Stop, .generation = state_.generation});
            break;
        case AppLifecycleEvent::Unload:
            Dispatch({.type = CommandType::Stop, .generation = state_.generation});
            ResetForUnload();
            break;
    }
    PublishState();
}

void Controller::HandleIntent(const Intent& intent) {
    switch (intent.type) {
        case IntentType::Capture:
            if (!state_.mounted || state_.mode != ViewMode::Camera ||
                state_.saving || !state_.preview_running ||
                state_.status_code == StatusCode::CameraStartupFailed ||
                state_.preview_frame == nullptr ||
                state_.preview_frame->data == nullptr ||
                state_.preview_frame->width <= 0 ||
                state_.preview_frame->height <= 0) {
                return;
            }
            Dispatch({.type = CommandType::Capture,
                      .generation = state_.generation});
            return;
        case IntentType::DeleteReview:
            if (state_.mode != ViewMode::Review || state_.saving) return;
            state_.mode = ViewMode::Camera;
            state_.frozen = false;
            state_.review_ready = false;
            state_.review_image.reset();
            Dispatch({.type = CommandType::DeleteReview, .generation = state_.generation});
            PublishState();
            return;
        case IntentType::SaveReview:
            if (state_.mode != ViewMode::Review || state_.saving) return;
            state_.saving = true;
            Dispatch({.type = CommandType::SaveReview, .generation = state_.generation});
            PublishState();
            return;
        case IntentType::OpenGallery:
            if (!state_.mounted || state_.saving) return;
            ++state_.generation;
            state_.mode = ViewMode::Gallery;
            state_.preview_running = false;
            state_.preview_frame.reset();
            state_.frozen = false;
            state_.review_ready = false;
            state_.review_image.reset();
            state_.gallery_loading = true;
            state_.gallery.clear();
            state_.thumbnails.clear();
            state_.thumbnail_failures.clear();
            state_.status_code = StatusCode::None;
            state_.status.clear();
            Dispatch({.type = CommandType::Stop, .generation = state_.generation});
            Dispatch({.type = CommandType::OpenGallery, .generation = state_.generation});
            PublishState();
            return;
        case IntentType::GalleryBack:
            ++state_.generation;
            state_.mode = ViewMode::Camera;
            // A stopped gallery stream cannot reuse its last frame as a
            // capture-ready preview while the new worker is starting.
            state_.preview_running = false;
            state_.preview_frame.reset();
            state_.frozen = false;
            state_.review_ready = false;
            state_.review_image.reset();
            state_.status_code = StatusCode::None;
            state_.status.clear();
            state_.gallery_loading = false;
            Dispatch({.type = CommandType::Start, .generation = state_.generation});
            PublishState();
            return;
        case IntentType::OpenViewer:
            if (intent.index >= state_.gallery.size()) return;
            ++state_.generation;
            state_.mode = ViewMode::Viewer;
            state_.viewer_index = intent.index;
            state_.viewer_loading = true;
            state_.viewer_image = nullptr;
            state_.status_code = StatusCode::None;
            state_.status.clear();
            Dispatch({.type = CommandType::OpenViewer,
                      .generation = state_.generation,
                      .index = intent.index,
                      .path = state_.gallery[intent.index].path});
            PublishState();
            return;
        case IntentType::ViewerBack:
            ++state_.generation;
            state_.mode = ViewMode::Gallery;
            state_.viewer_loading = false;
            state_.viewer_image = nullptr;
            state_.gallery_loading = true;
            state_.gallery.clear();
            state_.thumbnails.clear();
            state_.thumbnail_failures.clear();
            Dispatch({.type = CommandType::OpenGallery, .generation = state_.generation});
            PublishState();
            return;
        case IntentType::DeleteViewer:
            if (state_.mode != ViewMode::Viewer || state_.viewer_index >= state_.gallery.size()) {
                return;
            }
            ++state_.generation;
            Dispatch({.type = CommandType::DeleteViewer,
                      .generation = state_.generation,
                      .index = state_.viewer_index,
                      .path = state_.gallery[state_.viewer_index].path});
            return;
        case IntentType::NavigateBack:
            state_.preview_running = false;
            state_.preview_frame.reset();
            state_.frozen = false;
            state_.mounted = false;
            Dispatch({.type = CommandType::Stop, .generation = state_.generation});
            PublishState();
            return;
        case IntentType::LoadThumbnail:
            if (intent.index >= state_.gallery.size() ||
                intent.index >= state_.thumbnails.size() || state_.thumbnails[intent.index]) {
                return;
            }
            Dispatch({.type = CommandType::LoadThumbnail,
                      .generation = state_.generation,
                      .index = intent.index,
                      .path = state_.gallery[intent.index].path});
            return;
        case IntentType::PreviewDrawn:
            Dispatch({.type = CommandType::PreviewDrawn,
                      .generation = state_.generation,
                      .buffer_index = intent.buffer_index});
            return;
    }
}

void Controller::HandleEvent(const Event& event) {
    if (event.generation != 0 && event.generation != state_.generation) return;
    if (!state_.mounted && event.type != EventType::PreviewStopped) return;

    ViewState next = state_;
    switch (event.type) {
        case EventType::PreviewStarted:
            next.preview_running = true;
            next.preview_frame.reset();
            next.frozen = false;
            if (next.status_code == StatusCode::CameraStartupFailed) {
                next.status_code = StatusCode::None;
                next.status.clear();
            }
            break;
        case EventType::PreviewStopped:
            next.preview_running = false;
            next.preview_frame.reset();
            break;
        case EventType::PreviewFrameReady:
            next.preview_running = true;
            next.preview_frame = event.frame;
            break;
        case EventType::ReviewReady:
            if (!event.success || event.review_image == nullptr ||
                event.review_image->pixels == nullptr ||
                event.review_image->width == 0 || event.review_image->height == 0) {
                next.mode = ViewMode::Camera;
                next.frozen = false;
                next.review_ready = false;
                next.review_image.reset();
                next.status_code = StatusCode::BackendMessage;
                next.status = event.text.empty() ? "review_compose_failed" : event.text;
                break;
            }
            next.mode = ViewMode::Review;
            next.frozen = true;
            next.review_ready = true;
            next.review_image = event.review_image;
            next.preview_frame = event.frame;
            break;
        case EventType::SaveStarted:
            next.saving = true;
            next.mode = ViewMode::Camera;
            next.frozen = false;
            next.review_ready = false;
            next.review_image.reset();
            break;
        case EventType::SaveFinished:
            next.saving = false;
            next.mode = ViewMode::Camera;
            next.frozen = false;
            next.review_ready = false;
            next.review_image.reset();
            if (event.status_code == StatusCode::SaveSucceeded) {
                next.status_code = StatusCode::None;
                next.status.clear();
            } else {
                next.status_code = event.status_code;
                next.status = event.text;
            }
            break;
        case EventType::GalleryLoaded:
            next.mode = ViewMode::Gallery;
            next.gallery_loading = false;
            next.gallery_available = event.storage_available;
            next.gallery = event.photos;
            next.thumbnails.assign(next.gallery.size(), nullptr);
            next.thumbnail_failures.assign(next.gallery.size(), false);
            break;
        case EventType::ThumbnailReady:
            if (next.mode == ViewMode::Gallery &&
                event.index < next.thumbnails.size() &&
                event.index < next.gallery.size() &&
                event.path == next.gallery[event.index].path) {
                next.thumbnails[event.index] = event.image;
                if (event.index >= next.thumbnail_failures.size()) {
                    next.thumbnail_failures.resize(next.gallery.size(), false);
                }
                next.thumbnail_failures[event.index] = !event.success;
            }
            break;
        case EventType::ViewerReady:
            if (next.mode == ViewMode::Viewer &&
                event.index == next.viewer_index &&
                event.index < next.gallery.size() &&
                event.path == next.gallery[event.index].path) {
                next.viewer_loading = false;
                next.viewer_image = event.image;
                if (!event.success) {
                    next.status_code = StatusCode::BackendMessage;
                    next.status = "photo_decode_failed";
                }
            }
            break;
        case EventType::PhotoDeleted:
            if (event.success) {
                next.mode = ViewMode::Gallery;
                next.viewer_loading = false;
                next.viewer_image = nullptr;
                next.gallery = event.photos;
                next.thumbnails.assign(next.gallery.size(), nullptr);
                next.thumbnail_failures.assign(next.gallery.size(), false);
                next.status_code = StatusCode::None;
                next.status.clear();
            } else {
                next.viewer_loading = false;
                next.status_code = event.status_code;
                next.status = event.text;
            }
            break;
        case EventType::Status:
            next.status_code = event.status_code;
            next.status = event.text;
            if (event.status_code == StatusCode::CameraStartupFailed) {
                next.preview_running = false;
                next.preview_frame.reset();
            }
            break;
    }

    if (next == state_) return;
    state_ = std::move(next);
    PublishState();
}

}  // namespace agent_ui::camera
