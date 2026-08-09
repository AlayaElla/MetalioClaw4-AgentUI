#include "camera_image_decoder.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "camera_image_codec.h"

namespace agent_ui::camera {
namespace {

constexpr char kTag[] = "camera_image_decoder";
constexpr int kThumbnailWidth = 210;
constexpr int kThumbnailHeight = 144;
constexpr std::size_t kRequestQueueLength = 64;
constexpr std::uint32_t kWorkerStackBytes = 10 * 1024;

std::shared_ptr<uint8_t> OwnPsram(uint8_t* data) {
    return std::shared_ptr<uint8_t>(data, [](uint8_t* value) {
        if (value != nullptr) heap_caps_free(value);
    });
}

}  // namespace

struct ImageDecoder::Impl : std::enable_shared_from_this<ImageDecoder::Impl> {
    struct Request;

    EventSink sink;
    std::mutex sink_mutex;
    std::mutex queue_mutex;
    QueueHandle_t request_queue = nullptr;
    SemaphoreHandle_t worker_done = nullptr;
    TaskHandle_t worker_task = nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> latest_generation{0};
    std::atomic<uint32_t> request_epoch{0};

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        QueueHandle_t queue = nullptr;
        SemaphoreHandle_t done = nullptr;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue = request_queue;
            done = worker_done;
        }
        if (queue != nullptr && worker_task != nullptr) {
            Request* stop = nullptr;
            // The worker drops queued requests after stopping is set, so this
            // bounded send cannot strand the destructor behind decode work.
            xQueueSend(queue, &stop, portMAX_DELAY);
            if (done != nullptr) xSemaphoreTake(done, portMAX_DELAY);
        }
        if (queue != nullptr) {
            vQueueDelete(queue);
            request_queue = nullptr;
        }
        if (done != nullptr) {
            vSemaphoreDelete(done);
            worker_done = nullptr;
        }
    }

    void Emit(Event event) {
        EventSink callback;
        {
            std::lock_guard<std::mutex> lock(sink_mutex);
            callback = sink;
        }
        if (callback) callback(event);
    }

    std::shared_ptr<const DecodedImage> DecodeFile(const std::string& path,
                                                    bool thumbnail) {
        FILE* file = fopen(path.c_str(), "rb");
        if (file == nullptr || fseek(file, 0, SEEK_END) != 0) {
            if (file != nullptr) fclose(file);
            return nullptr;
        }
        const long encoded_size_long = ftell(file);
        if (encoded_size_long <= 0 || fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            return nullptr;
        }
        const std::size_t encoded_size = static_cast<std::size_t>(encoded_size_long);
        auto* encoded = static_cast<uint8_t*>(
            heap_caps_malloc(encoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (encoded == nullptr) {
            fclose(file);
            return nullptr;
        }
        const std::size_t read_size = fread(encoded, 1, encoded_size, file);
        fclose(file);
        if (read_size != encoded_size) {
            heap_caps_free(encoded);
            return nullptr;
        }

        uint8_t* decoded = nullptr;
        std::size_t decoded_size = 0;
        std::size_t source_width = 0;
        std::size_t source_height = 0;
        std::size_t source_stride = 0;
        codec::DecodedBuffer decoded_buffer;
        const bool decoded_ok = codec::DecodeJpeg(encoded, encoded_size,
                                                  decoded_buffer);
        heap_caps_free(encoded);
        if (!decoded_ok) {
            return nullptr;
        }
        decoded = decoded_buffer.data;
        decoded_size = decoded_buffer.size;
        source_width = decoded_buffer.width;
        source_height = decoded_buffer.height;
        source_stride = decoded_buffer.stride;

        auto image = std::make_shared<DecodedImage>();
        if (!thumbnail) {
            image->pixels = OwnPsram(decoded);
            image->data_size = decoded_size;
            image->width = source_width;
            image->height = source_height;
            image->stride = source_stride;
            return image;
        }

        const std::size_t thumbnail_size =
            static_cast<std::size_t>(kThumbnailWidth) * kThumbnailHeight * 2;
        auto* thumbnail_pixels = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(64, thumbnail_size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (thumbnail_pixels == nullptr) {
            heap_caps_free(decoded);
            return nullptr;
        }
        const uint32_t x_step =
            static_cast<uint32_t>((source_width << 16) / kThumbnailWidth);
        const uint32_t y_step =
            static_cast<uint32_t>((source_height << 16) / kThumbnailHeight);
        auto* destination = reinterpret_cast<uint16_t*>(thumbnail_pixels);
        uint32_t y_acc = 0;
        for (int y = 0; y < kThumbnailHeight; ++y) {
            const std::size_t source_y =
                std::min<std::size_t>(y_acc >> 16, source_height - 1);
            const auto* source_row = reinterpret_cast<const uint16_t*>(
                decoded + source_y * source_stride);
            uint32_t x_acc = 0;
            for (int x = 0; x < kThumbnailWidth; ++x) {
                const std::size_t source_x =
                    std::min<std::size_t>(x_acc >> 16, source_width - 1);
                destination[y * kThumbnailWidth + x] = source_row[source_x];
                x_acc += x_step;
            }
            y_acc += y_step;
        }
        heap_caps_free(decoded);
        image->pixels = OwnPsram(thumbnail_pixels);
        image->data_size = thumbnail_size;
        image->width = kThumbnailWidth;
        image->height = kThumbnailHeight;
        image->stride = kThumbnailWidth * 2;
        return image;
    }

    struct Request {
        uint32_t generation = 0;
        uint32_t epoch = 0;
        std::size_t index = 0;
        std::string path;
        bool thumbnail = false;
    };

    bool IsCurrent(const Request& request) const {
        return !stopping.load(std::memory_order_acquire) &&
               request.generation == latest_generation.load(std::memory_order_acquire) &&
               request.epoch == request_epoch.load(std::memory_order_acquire);
    }

    static void WorkerEntry(void* arg) {
        auto* owner = static_cast<Impl*>(arg);
        if (owner == nullptr) {
            vTaskDelete(nullptr);
            return;
        }
        while (true) {
            Request* raw_request = nullptr;
            if (owner->request_queue == nullptr ||
                xQueueReceive(owner->request_queue, &raw_request,
                              portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (raw_request == nullptr) break;
            std::unique_ptr<Request> request(raw_request);
            if (!owner->IsCurrent(*request)) continue;

            auto image = owner->DecodeFile(request->path, request->thumbnail);
            if (!owner->IsCurrent(*request)) continue;
            Event event;
            event.type = request->thumbnail ? EventType::ThumbnailReady
                                            : EventType::ViewerReady;
            event.generation = request->generation;
            event.index = request->index;
            event.path = request->path;
            event.success = image != nullptr;
            event.image = std::move(image);
            if (!event.success) {
                // Keep the UI placeholder generic; the log carries only the
                // request identity needed to diagnose a failed decode and
                // deliberately omits the user's file path.
                ESP_LOGW(kTag, "%s decode failed (generation=%u,index=%u)",
                         request->thumbnail ? "thumbnail" : "viewer",
                         static_cast<unsigned>(request->generation),
                         static_cast<unsigned>(request->index));
                event.text = request->thumbnail ? "thumbnail_decode_failed"
                                                 : "viewer_decode_failed";
            }
            owner->Emit(std::move(event));
        }
        if (owner->worker_done != nullptr) xSemaphoreGive(owner->worker_done);
        vTaskDelete(nullptr);
    }

    void Start(uint32_t generation, std::size_t index,
               const std::string& path, bool thumbnail) {
        latest_generation.store(generation, std::memory_order_release);
        auto* request = new Request{
            .generation = generation,
            .epoch = request_epoch.load(std::memory_order_acquire),
            .index = index,
            .path = path,
            .thumbnail = thumbnail,
        };

        std::lock_guard<std::mutex> lock(queue_mutex);
        if (stopping.load(std::memory_order_acquire)) {
            delete request;
            return;
        }
        if (request_queue == nullptr) {
            request_queue = xQueueCreate(kRequestQueueLength, sizeof(Request*));
            worker_done = xSemaphoreCreateBinary();
            if (request_queue == nullptr || worker_done == nullptr ||
                xTaskCreate(WorkerEntry, "cam_decode", kWorkerStackBytes, this,
                            tskIDLE_PRIORITY + 1, &worker_task) != pdPASS) {
                if (request_queue != nullptr) {
                    vQueueDelete(request_queue);
                    request_queue = nullptr;
                }
                if (worker_done != nullptr) {
                    vSemaphoreDelete(worker_done);
                    worker_done = nullptr;
                }
                worker_task = nullptr;
                delete request;
                Event event;
                event.type = thumbnail ? EventType::ThumbnailReady
                                       : EventType::ViewerReady;
                event.generation = generation;
                event.index = index;
                event.path = path;
                event.success = false;
                event.text = thumbnail ? "thumbnail_decode_unavailable"
                                       : "viewer_decode_unavailable";
                Emit(std::move(event));
                return;
            }
        }
        Request* queued = request;
        // A gallery session has at most 48 entries and the queue intentionally
        // has headroom. If a caller floods it across sessions, fail that card
        // explicitly instead of blocking the LVGL command path indefinitely.
        if (xQueueSend(request_queue, &queued, 0) != pdTRUE) {
            delete request;
            Event event;
            event.type = thumbnail ? EventType::ThumbnailReady
                                   : EventType::ViewerReady;
            event.generation = generation;
            event.index = index;
            event.path = path;
            event.success = false;
            event.text = thumbnail ? "thumbnail_decode_queue_full"
                                   : "viewer_decode_queue_full";
            Emit(std::move(event));
        }
    }

    void Cancel() { request_epoch.fetch_add(1, std::memory_order_acq_rel); }
};

ImageDecoder::ImageDecoder() : impl_(std::make_shared<Impl>()) {}

ImageDecoder::~ImageDecoder() = default;

void ImageDecoder::SetEventSink(EventSink sink) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->sink_mutex);
    impl_->sink = std::move(sink);
}

void ImageDecoder::Cancel() {
    if (impl_) impl_->Cancel();
}

void ImageDecoder::DecodeThumbnail(uint32_t generation, std::size_t index,
                                   const std::string& path) {
    if (impl_) impl_->Start(generation, index, path, true);
}

void ImageDecoder::DecodeViewer(uint32_t generation, std::size_t index,
                                const std::string& path) {
    if (impl_) impl_->Start(generation, index, path, false);
}

}  // namespace agent_ui::camera
