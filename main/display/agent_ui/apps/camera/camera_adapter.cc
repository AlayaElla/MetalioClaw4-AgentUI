#include "camera_adapter.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include "camera_capture_backend.h"
#include "camera_gallery_repository.h"
#include "camera_image_codec.h"
#include "camera_image_decoder.h"
#include "SdCardManager.hpp"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace agent_ui::camera {
namespace {

constexpr uint8_t kSaveJpegQuality = 80;

}  // namespace

struct Adapter::Impl : std::enable_shared_from_this<Adapter::Impl> {
    CaptureBackend capture;
    GalleryRepository gallery;
    ImageDecoder decoder;
    EventSink sink;
    std::mutex sink_mutex;
    std::atomic<bool> active{true};
    std::atomic<uint32_t> generation{0};

    void Emit(Event event) {
        if (!active.load(std::memory_order_acquire)) return;
        if (event.generation != 0 &&
            event.generation != generation.load(std::memory_order_acquire)) {
            return;
        }
        EventSink callback;
        {
            std::lock_guard<std::mutex> lock(sink_mutex);
            callback = sink;
        }
        if (callback) callback(event);
    }

    void SetSinks() {
        capture.SetEventSink([weak = std::weak_ptr<Impl>(shared_from_this())](
                                  const Event& event) {
            if (auto owner = weak.lock()) owner->Emit(event);
        });
        decoder.SetEventSink([weak = std::weak_ptr<Impl>(shared_from_this())](
                                  const Event& event) {
            if (auto owner = weak.lock()) owner->Emit(event);
        });
    }

    struct SaveRequest {
        std::shared_ptr<Impl> owner;
        std::shared_ptr<const PreviewFrame> frame;
        uint32_t generation = 0;
    };

    static void SaveTask(void* argument) {
        std::unique_ptr<SaveRequest> request(static_cast<SaveRequest*>(argument));
        if (request == nullptr || !request->owner || !request->frame) {
            vTaskDelete(nullptr);
            return;
        }
        auto owner = std::move(request->owner);
        const bool valid_frame = request->frame->data != nullptr &&
                                 request->frame->width > 0 &&
                                 request->frame->height > 0 &&
                                 request->frame->width <= UINT16_MAX &&
                                 request->frame->height <= UINT16_MAX;
        const size_t rgb565_size =
            valid_frame
                ? static_cast<size_t>(request->frame->width) *
                      request->frame->height * sizeof(uint16_t)
                : 0;
        std::vector<uint8_t> jpeg;
        const bool encoded = valid_frame && codec::EncodeRgb565(
            request->frame->data, rgb565_size,
            static_cast<uint16_t>(request->frame->width),
            static_cast<uint16_t>(request->frame->height),
            kSaveJpegQuality, jpeg);
        std::string path;
        const bool saved = encoded && owner->gallery.WriteJpeg(jpeg, &path);
        owner->capture.Resume();
        Event finished;
        finished.type = EventType::SaveFinished;
        finished.generation = request->generation;
        finished.success = saved;
        finished.status_code = saved ? StatusCode::SaveSucceeded
                                      : StatusCode::SaveFailed;
        finished.text = saved ? "" : "save_failed";
        owner->Emit(std::move(finished));
        owner.reset();
        request.reset();
        vTaskDelete(nullptr);
    }

    void Save(const Command& command) {
        auto frame = capture.CopyCurrentFrame(command.generation);
        if (!frame) {
            Event event;
            event.type = EventType::SaveFinished;
            event.generation = command.generation;
            event.status_code = StatusCode::SaveFailed;
            event.text = "save_failed";
            Emit(std::move(event));
            return;
        }
        auto* request = new SaveRequest{
            .owner = shared_from_this(),
            .frame = std::move(frame),
            .generation = command.generation,
        };
        Event started;
        started.type = EventType::SaveStarted;
        started.generation = command.generation;
        Emit(std::move(started));
        if (xTaskCreate(SaveTask, "cam_save", 10 * 1024, request,
                        tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
            delete request;
            capture.Resume();
            Event event;
            event.type = EventType::SaveFinished;
            event.generation = command.generation;
            event.status_code = StatusCode::SaveFailed;
            event.text = "save_failed";
            Emit(std::move(event));
        }
    }

    void Execute(const Command& command) {
        generation.store(command.generation, std::memory_order_release);
        switch (command.type) {
            case CommandType::Start:
                active.store(true, std::memory_order_release);
                capture.Start(command.generation);
                break;
            case CommandType::Stop:
                decoder.Cancel();
                capture.Stop();
                break;
            case CommandType::Capture:
                capture.Capture();
                break;
            case CommandType::DeleteReview:
                capture.Resume();
                break;
            case CommandType::SaveReview:
                Save(command);
                break;
            case CommandType::OpenGallery: {
                Event event;
                event.type = EventType::GalleryLoaded;
                event.generation = command.generation;
                event.storage_available = SdCardManager::GetInstance().IsMounted();
                event.photos = gallery.List();
                event.success = event.storage_available;
                Emit(std::move(event));
                break;
            }
            case CommandType::OpenViewer:
                decoder.DecodeViewer(command.generation, command.index, command.path);
                break;
            case CommandType::DeleteViewer: {
                const bool deleted = gallery.Delete(command.path);
                Event event;
                event.type = EventType::PhotoDeleted;
                event.generation = command.generation;
                event.success = deleted;
                event.status_code = deleted ? StatusCode::None
                                            : StatusCode::DeleteFailed;
                event.photos = gallery.List();
                event.text = deleted ? "" : "delete_failed";
                Emit(std::move(event));
                break;
            }
            case CommandType::LoadThumbnail:
                decoder.DecodeThumbnail(command.generation, command.index,
                                        command.path);
                break;
            case CommandType::PreviewDrawn:
                capture.AcknowledgeFrame(command.generation, command.buffer_index);
                break;
            case CommandType::SetEffect:
                capture.SetEffect(command.effect_style, command.dark_mode);
                break;
        }
    }
};

Adapter::Adapter() : impl_(std::make_shared<Impl>()) { impl_->SetSinks(); }

Adapter::~Adapter() {
    if (impl_) {
        impl_->active.store(false, std::memory_order_release);
        impl_->capture.Stop();
        std::lock_guard<std::mutex> lock(impl_->sink_mutex);
        impl_->sink = nullptr;
    }
    impl_.reset();
}

void Adapter::SetEventSink(EventSink sink) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->sink_mutex);
    impl_->sink = std::move(sink);
}

void Adapter::Execute(const Command& command) {
    if (impl_) impl_->Execute(command);
}

}  // namespace agent_ui::camera
