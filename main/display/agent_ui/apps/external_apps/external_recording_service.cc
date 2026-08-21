#include "external_recording_service.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include "application.h"
#include "audio/audio_service.h"
#include "device_state.h"

namespace agent_ui::external_apps {
namespace {

constexpr char kTag[] = "ExternalRecording";
constexpr char kRecordingRoot[] = "/sdcard/Recordings";
constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kBytesPerSecond = kSampleRate * sizeof(int16_t);
constexpr uint32_t kDefaultDurationMs = 5 * 60 * 1000;
constexpr uint32_t kMinimumDurationMs = 1000;
constexpr uint32_t kMaximumDurationMs = 10 * 60 * 1000;
constexpr size_t kPsramBufferBytes = 64 * 1024;
constexpr size_t kInternalBufferBytes = 16 * 1024;
constexpr size_t kWriterChunkBytes = 4096;
RecordingService* s_existing_service = nullptr;

void PutLe16(uint8_t* output, uint16_t value) {
    output[0] = static_cast<uint8_t>(value & 0xff);
    output[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void PutLe32(uint8_t* output, uint32_t value) {
    PutLe16(output, static_cast<uint16_t>(value & 0xffff));
    PutLe16(output + 2, static_cast<uint16_t>(value >> 16));
}

bool WriteWavHeader(FILE* file, uint32_t data_bytes) {
    if (file == nullptr || std::fseek(file, 0, SEEK_SET) != 0) return false;
    uint8_t header[44]{};
    std::memcpy(header, "RIFF", 4);
    PutLe32(header + 4, 36 + data_bytes);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    PutLe32(header + 16, 16);
    PutLe16(header + 20, 1);
    PutLe16(header + 22, 1);
    PutLe32(header + 24, kSampleRate);
    PutLe32(header + 28, kBytesPerSecond);
    PutLe16(header + 32, sizeof(int16_t));
    PutLe16(header + 34, 16);
    std::memcpy(header + 36, "data", 4);
    PutLe32(header + 40, data_bytes);
    return std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

std::string SanitizeBaseName(const char* input) {
    std::string result;
    if (input == nullptr) return result;
    const size_t length = strnlen(input, 96);
    if (length == 0 || length == 96) return result;
    result.reserve(std::min<size_t>(length, 48));
    for (size_t i = 0; i < length && result.size() < 48; ++i) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            result.push_back(static_cast<char>(ch));
        } else if (std::isspace(ch)) {
            if (result.empty() || result.back() != '_') result.push_back('_');
        }
    }
    return result;
}

std::string DefaultBaseName() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    char name[40]{};
    if (localtime_r(&now, &local) != nullptr && local.tm_year >= 125) {
        std::strftime(name, sizeof(name), "REC_%Y%m%d_%H%M%S", &local);
    } else {
        std::snprintf(name, sizeof(name), "REC_%lld",
                      static_cast<long long>(now));
    }
    return name;
}

std::string UniquePath(const std::string& requested) {
    const std::string base = requested.empty() ? DefaultBaseName() : requested;
    for (unsigned suffix = 0; suffix < 1000; ++suffix) {
        char path[METALIO_APP_RECORDING_PATH_BYTES]{};
        if (suffix == 0) {
            std::snprintf(path, sizeof(path), "%s/%s.wav", kRecordingRoot,
                          base.c_str());
        } else {
            std::snprintf(path, sizeof(path), "%s/%s_%03u.wav",
                          kRecordingRoot, base.c_str(), suffix);
        }
        if (access(path, F_OK) != 0) return path;
    }
    return {};
}

}  // namespace

struct RecordingService::Impl {
    mutable std::mutex mutex;
    void* owner = nullptr;
    FILE* file = nullptr;
    StreamBufferHandle_t stream = nullptr;
    TaskHandle_t writer_task = nullptr;
    std::atomic<bool> accepting{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> discard_requested{false};
    std::atomic<uint32_t> dropped_frames{0};
    std::atomic<uint8_t> peak_percent{0};
    metalio_app_recording_state_t state = METALIO_APP_RECORDING_IDLE;
    metalio_app_recording_error_t error = METALIO_APP_RECORDING_OK;
    uint32_t max_data_bytes = 0;
    std::atomic<uint32_t> data_bytes{0};
    std::string path;

    AudioService& audio() {
        return Application::GetInstance().GetAudioService();
    }

    void OnPcm(const int16_t* samples, size_t count) {
        if (!accepting.load(std::memory_order_acquire) || samples == nullptr ||
            count == 0) {
            return;
        }
        uint32_t maximum = 0;
        for (size_t i = 0; i < count; ++i) {
            const int32_t value = samples[i];
            const uint32_t magnitude = static_cast<uint32_t>(
                value < 0 ? -value : value);
            maximum = std::max(maximum, magnitude);
        }
        const uint8_t percent = static_cast<uint8_t>(
            std::min<uint32_t>(100, maximum * 100 / 32767));
        peak_percent.store(percent, std::memory_order_relaxed);

        StreamBufferHandle_t active = stream;
        const size_t bytes = count * sizeof(int16_t);
        if (active == nullptr || xStreamBufferSpacesAvailable(active) < bytes ||
            xStreamBufferSend(active, samples, bytes, 0) != bytes) {
            dropped_frames.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static void WriterEntry(void* argument) {
        static_cast<Impl*>(argument)->WriterMain();
    }

    void DetachCapture() {
        accepting.store(false, std::memory_order_release);
        audio().SetExternalRecordingPcmCallback({});
        audio().SetExternalRecordingActive(false);
    }

    void WriterMain() {
        uint8_t buffer[kWriterChunkBytes];
        bool capture_detached = false;
        bool write_failed = false;
        uint32_t written_bytes = 0;

        while (true) {
            if (stop_requested.load(std::memory_order_acquire) &&
                !capture_detached) {
                DetachCapture();
                capture_detached = true;
            }

            const size_t received = xStreamBufferReceive(
                stream, buffer, sizeof(buffer), pdMS_TO_TICKS(80));
            if (received > 0 && !write_failed) {
                const uint32_t remaining = max_data_bytes - written_bytes;
                const size_t to_write = std::min<size_t>(received, remaining);
                if (to_write > 0 &&
                    std::fwrite(buffer, 1, to_write, file) != to_write) {
                    write_failed = true;
                    accepting.store(false, std::memory_order_release);
                    stop_requested.store(true, std::memory_order_release);
                } else {
                    written_bytes += static_cast<uint32_t>(to_write);
                    data_bytes.store(written_bytes, std::memory_order_relaxed);
                }
                if (written_bytes >= max_data_bytes) {
                    accepting.store(false, std::memory_order_release);
                    stop_requested.store(true, std::memory_order_release);
                }
            }

            if (stop_requested.load(std::memory_order_acquire) &&
                capture_detached && xStreamBufferBytesAvailable(stream) == 0) {
                break;
            }
        }

        if (!capture_detached) DetachCapture();
        const bool discard = discard_requested.load(std::memory_order_acquire);
        bool finalized = !write_failed && !discard &&
                         WriteWavHeader(file, written_bytes) &&
                         std::fflush(file) == 0;
        std::fclose(file);
        file = nullptr;
        if (!finalized) std::remove(path.c_str());
        vStreamBufferDeleteWithCaps(stream);
        stream = nullptr;

        metalio_app_recording_state_t final_state;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (discard) {
                state = METALIO_APP_RECORDING_CANCELLED;
                error = METALIO_APP_RECORDING_ERROR_CANCELLED;
                path.clear();
            } else if (!finalized) {
                state = METALIO_APP_RECORDING_ERROR;
                error = write_failed ? METALIO_APP_RECORDING_ERROR_WRITE
                                     : METALIO_APP_RECORDING_ERROR_STORAGE;
                path.clear();
            } else {
                state = METALIO_APP_RECORDING_COMPLETED;
                error = METALIO_APP_RECORDING_OK;
            }
            final_state = state;
            writer_task = nullptr;
        }
        ESP_LOGI(kTag, "recording finished state=%d bytes=%u dropped=%u",
                 static_cast<int>(final_state),
                 static_cast<unsigned>(written_bytes),
                 static_cast<unsigned>(
                     dropped_frames.load(std::memory_order_relaxed)));
        vTaskDelete(nullptr);
    }

    int RequestStop(void* requested_owner, bool discard) {
        std::lock_guard<std::mutex> lock(mutex);
        if (requested_owner == nullptr || requested_owner != owner ||
            writer_task == nullptr ||
            (state != METALIO_APP_RECORDING_RECORDING &&
             state != METALIO_APP_RECORDING_STOPPING)) {
            return METALIO_APP_RECORDING_ERROR_INVALID;
        }
        if (discard) discard_requested.store(true, std::memory_order_release);
        accepting.store(false, std::memory_order_release);
        stop_requested.store(true, std::memory_order_release);
        state = METALIO_APP_RECORDING_STOPPING;
        return METALIO_APP_RECORDING_OK;
    }
};

RecordingService& RecordingService::Get() {
    static RecordingService instance;
    return instance;
}

RecordingService::RecordingService() : impl_(new Impl()) {
    s_existing_service = this;
}

RecordingService* RecordingService::Existing() {
    return s_existing_service;
}

int RecordingService::Start(
        void* owner, const metalio_app_recording_config_t* config) {
    if (owner == nullptr) return METALIO_APP_RECORDING_ERROR_INVALID;
    Application& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle || app.IsHermesVoiceBusy() ||
        app.IsCodexVoiceCaptureActive()) {
        return METALIO_APP_RECORDING_ERROR_BUSY;
    }

    uint32_t duration_ms = config != nullptr ? config->max_duration_ms : 0;
    if (duration_ms == 0) duration_ms = kDefaultDurationMs;
    duration_ms = std::clamp(duration_ms, kMinimumDurationMs,
                             kMaximumDurationMs);
    const std::string requested =
        SanitizeBaseName(config != nullptr ? config->file_name : nullptr);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->writer_task != nullptr) return METALIO_APP_RECORDING_ERROR_BUSY;
    if (access("/sdcard", F_OK) != 0 ||
        (mkdir(kRecordingRoot, 0755) != 0 && errno != EEXIST)) {
        return METALIO_APP_RECORDING_ERROR_STORAGE;
    }
    const std::string path = UniquePath(requested);
    if (path.empty()) return METALIO_APP_RECORDING_ERROR_STORAGE;
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr || !WriteWavHeader(file, 0)) {
        if (file != nullptr) std::fclose(file);
        std::remove(path.c_str());
        return METALIO_APP_RECORDING_ERROR_STORAGE;
    }

    StreamBufferHandle_t stream = xStreamBufferCreateWithCaps(
        kPsramBufferBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stream == nullptr) {
        stream = xStreamBufferCreateWithCaps(
            kInternalBufferBytes, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (stream == nullptr) {
        std::fclose(file);
        std::remove(path.c_str());
        return METALIO_APP_RECORDING_ERROR_AUDIO;
    }

    impl_->owner = owner;
    impl_->file = file;
    impl_->stream = stream;
    impl_->path = path;
    impl_->max_data_bytes = static_cast<uint32_t>(
        (static_cast<uint64_t>(duration_ms) * kBytesPerSecond) / 1000);
    impl_->data_bytes.store(0, std::memory_order_relaxed);
    impl_->state = METALIO_APP_RECORDING_RECORDING;
    impl_->error = METALIO_APP_RECORDING_OK;
    impl_->dropped_frames.store(0, std::memory_order_relaxed);
    impl_->peak_percent.store(0, std::memory_order_relaxed);
    impl_->stop_requested.store(false, std::memory_order_release);
    impl_->discard_requested.store(false, std::memory_order_release);
    impl_->accepting.store(false, std::memory_order_release);

    if (xTaskCreate(Impl::WriterEntry, "external_record", 6144, impl_, 3,
                    &impl_->writer_task) != pdPASS) {
        impl_->writer_task = nullptr;
        vStreamBufferDeleteWithCaps(stream);
        impl_->stream = nullptr;
        std::fclose(file);
        impl_->file = nullptr;
        std::remove(path.c_str());
        impl_->state = METALIO_APP_RECORDING_ERROR;
        impl_->error = METALIO_APP_RECORDING_ERROR_AUDIO;
        impl_->path.clear();
        return METALIO_APP_RECORDING_ERROR_AUDIO;
    }

    impl_->audio().SetExternalRecordingPcmCallback(
        [recording = impl_](const int16_t* samples, size_t count) {
            recording->OnPcm(samples, count);
        });
    impl_->accepting.store(true, std::memory_order_release);
    impl_->audio().SetExternalRecordingActive(true);
    if (!impl_->audio().IsAudioProcessorRunning()) {
        impl_->discard_requested.store(true, std::memory_order_release);
        impl_->accepting.store(false, std::memory_order_release);
        impl_->stop_requested.store(true, std::memory_order_release);
        impl_->state = METALIO_APP_RECORDING_STOPPING;
        return METALIO_APP_RECORDING_ERROR_AUDIO;
    }
    ESP_LOGI(kTag, "recording started path=%s max_ms=%u", path.c_str(),
             static_cast<unsigned>(duration_ms));
    return METALIO_APP_RECORDING_OK;
}

int RecordingService::Stop(void* owner) {
    return impl_->RequestStop(owner, false);
}

int RecordingService::Cancel(void* owner) {
    return impl_->RequestStop(owner, true);
}

int RecordingService::GetStatus(
        void* owner, metalio_app_recording_status_t* status) const {
    if (owner == nullptr || status == nullptr) {
        return METALIO_APP_RECORDING_ERROR_INVALID;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (owner != impl_->owner) return METALIO_APP_RECORDING_ERROR_INVALID;
    *status = {};
    status->state = impl_->state;
    status->error = impl_->error;
    status->data_bytes =
        impl_->data_bytes.load(std::memory_order_relaxed);
    status->duration_ms = static_cast<uint32_t>(
        (static_cast<uint64_t>(status->data_bytes) * 1000) / kBytesPerSecond);
    status->dropped_frames =
        impl_->dropped_frames.load(std::memory_order_relaxed);
    status->peak_percent =
        impl_->peak_percent.load(std::memory_order_relaxed);
    std::snprintf(status->path, sizeof(status->path), "%s",
                  impl_->path.c_str());
    return METALIO_APP_RECORDING_OK;
}

void RecordingService::SuspendOwner(void* owner) {
    if (owner != nullptr) impl_->RequestStop(owner, false);
}

void RecordingService::UnloadOwner(void* owner) {
    if (owner != nullptr) impl_->RequestStop(owner, false);
}

bool RecordingService::ResetForAppLaunch(uint32_t timeout_ms) {
    if (impl_ == nullptr) return true;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->writer_task != nullptr) {
            // This is a system transition, so it must not depend on the old
            // Runtime::State pointer still being a valid owner token.
            impl_->accepting.store(false, std::memory_order_release);
            impl_->stop_requested.store(true, std::memory_order_release);
            impl_->state = METALIO_APP_RECORDING_STOPPING;
        }
    }

    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->writer_task == nullptr) {
                impl_->owner = nullptr;
                impl_->state = METALIO_APP_RECORDING_IDLE;
                impl_->error = METALIO_APP_RECORDING_OK;
                impl_->stop_requested.store(false, std::memory_order_relaxed);
                impl_->discard_requested.store(false,
                                               std::memory_order_relaxed);
                impl_->dropped_frames.store(0, std::memory_order_relaxed);
                impl_->peak_percent.store(0, std::memory_order_relaxed);
                impl_->data_bytes.store(0, std::memory_order_relaxed);
                impl_->max_data_bytes = 0;
                std::string().swap(impl_->path);
                return true;
            }
        }
        if (static_cast<TickType_t>(xTaskGetTickCount() - started) >=
            timeout_ticks) {
            ESP_LOGE(kTag, "recording reset timed out");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace agent_ui::external_apps
