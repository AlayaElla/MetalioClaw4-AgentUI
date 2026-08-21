#include "external_media_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_crt_bundle.h"
#include "esp_fourcc.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_heap_caps.h"
#include "esp_hls_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"

#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
#include "esp_gmf_rate_cvt.h"
#endif

namespace agent_ui::external_apps {
namespace {

constexpr char kTag[] = "ExternalMedia";
constexpr int kOutputSampleRate = 16000;
constexpr int kFftN = 256;
constexpr size_t kPcmRingSize = 2048;
constexpr float kAttack = 0.55f;
constexpr float kRelease = 0.18f;
constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kMaxUrlBytes = 1024;
constexpr std::array<const char*, 6> kHlsPipelineLogTags = {
    "ESP_GMF_TASK", "ESP_GMF_BLOCK", "ESP_GMF_HTTP",
    "HLS_IO",       "HLS_IO_HELPER", "HLS_Playlist",
};
MediaService* s_existing_service = nullptr;

enum class LifeState : uint8_t {
    Idle,
    Starting,
    Running,
    Stopping,
    Suspended,
};

bool IsSupportedUrl(const char* url) {
    if (url == nullptr) return false;
    const size_t length = strnlen(url, kMaxUrlBytes + 1);
    if (length == 0 || length > kMaxUrlBytes) return false;
    return strncasecmp(url, "http://", 7) == 0 ||
           strncasecmp(url, "https://", 8) == 0;
}

}  // namespace

struct MediaService::Impl {
    SemaphoreHandle_t mutex = nullptr;
    std::atomic<LifeState> life{LifeState::Idle};
    std::atomic<MediaState> public_state{MediaState::Idle};
    std::atomic<bool> want_play{false};
    std::atomic<bool> shutdown{false};
    std::atomic<bool> stop_pending{false};
    std::atomic<bool> stop_worker_busy{false};
    std::atomic<bool> spectrum_run{false};
    std::atomic<bool> reported_playing{false};
    std::atomic<int> pcm_channels{2};
    std::atomic<uint32_t> generation{0};

    std::atomic<void*> owner{nullptr};
    std::atomic<void*> pending_owner{nullptr};
    std::string url;
    std::string pending_url;
    bool suspend_after_stop = false;
    bool resume_after_suspend = false;
    bool resume_requested = false;
    bool audio_acquired = false;

    TaskHandle_t play_task = nullptr;
    TaskHandle_t start_task = nullptr;
    TaskHandle_t stop_task = nullptr;
    TaskHandle_t player_stop_task = nullptr;
    TaskHandle_t spectrum_task = nullptr;
    esp_asp_handle_t player = nullptr;
    AudioCodec* codec = nullptr;
    std::vector<int16_t> pcm_buffer;
    bool media_adapter_ready = false;
    uint32_t hls_last_format = 0;
    std::array<esp_log_level_t, kHlsPipelineLogTags.size()>
        saved_hls_log_levels{};
    bool hls_logs_limited = false;

    void LimitHlsLogsToErrors() {
        if (hls_logs_limited) return;
        for (size_t index = 0; index < kHlsPipelineLogTags.size(); ++index) {
            saved_hls_log_levels[index] =
                esp_log_level_get(kHlsPipelineLogTags[index]);
            esp_log_level_set(kHlsPipelineLogTags[index], ESP_LOG_ERROR);
        }
        hls_logs_limited = true;
    }

    void RestoreHlsLogLevels() {
        if (!hls_logs_limited) return;
        for (size_t index = 0; index < kHlsPipelineLogTags.size(); ++index) {
            esp_log_level_set(kHlsPipelineLogTags[index],
                              saved_hls_log_levels[index]);
        }
        hls_logs_limited = false;
    }

    alignas(16) int16_t pcm_ring[kPcmRingSize]{};
    std::atomic<uint32_t> pcm_write{0};
    std::atomic<uint32_t> pcm_read{0};
    portMUX_TYPE spectrum_mux = portMUX_INITIALIZER_UNLOCKED;
    uint8_t spectrum_levels[MediaService::kSpectrumBandCount]{};

    bool Lock(TickType_t timeout = pdMS_TO_TICKS(1000)) const {
        return mutex != nullptr && xSemaphoreTake(mutex, timeout) == pdTRUE;
    }

    void Unlock() const {
        if (mutex != nullptr) xSemaphoreGive(mutex);
    }

    bool OwnerMatches(void* candidate) const {
        return candidate != nullptr &&
               owner.load(std::memory_order_acquire) == candidate;
    }

    void ClearSpectrum() {
        portENTER_CRITICAL(&spectrum_mux);
        std::memset(spectrum_levels, 0, sizeof(spectrum_levels));
        portEXIT_CRITICAL(&spectrum_mux);
    }

    void PublishSpectrum(const float* levels) {
        portENTER_CRITICAL(&spectrum_mux);
        for (size_t index = 0; index < MediaService::kSpectrumBandCount;
             ++index) {
            const float value = std::clamp(levels[index], 0.0f, 1.0f);
            spectrum_levels[index] =
                static_cast<uint8_t>(value * 255.0f + 0.5f);
        }
        portEXIT_CRITICAL(&spectrum_mux);
    }

    void PushPcm(const int16_t* samples, int count) {
        if (samples == nullptr || count <= 0) return;
        uint32_t write = pcm_write.load(std::memory_order_relaxed);
        for (int index = 0; index < count; ++index) {
            pcm_ring[write & (kPcmRingSize - 1)] = samples[index];
            ++write;
        }
        pcm_write.store(write, std::memory_order_release);
        const uint32_t read = pcm_read.load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(write - read) > kPcmRingSize) {
            pcm_read.store(write - kPcmRingSize, std::memory_order_relaxed);
        }
    }

    bool PopPcm(int16_t* output, int count) {
        if (output == nullptr || count <= 0) return false;
        const uint32_t write = pcm_write.load(std::memory_order_acquire);
        uint32_t read = pcm_read.load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(write - read) <
            static_cast<uint32_t>(count)) {
            return false;
        }
        for (int index = 0; index < count; ++index) {
            output[index] = pcm_ring[read & (kPcmRingSize - 1)];
            ++read;
        }
        pcm_read.store(read, std::memory_order_release);
        return true;
    }

    static void SpectrumTaskEntry(void* argument) {
        static_cast<Impl*>(argument)->SpectrumTaskMain();
    }

    void SpectrumTaskMain() {
        // The P4 PIE implementation in gmf_fft can fault inside its hardware
        // loop while the GMF decoder is concurrently active. The UI needs only
        // twelve display bands, so a bank of Goertzel filters is both smaller
        // and avoids sharing that vector-assembly path with audio decoding.
        float window[kFftN];
        for (int index = 0; index < kFftN; ++index) {
            window[index] =
                0.5f * (1.0f - std::cos(2.0f * kPi *
                                        index / (kFftN - 1)));
        }

        float coefficients[MediaService::kSpectrumBandCount];
        constexpr float minimum_hz = 60.0f;
        constexpr float maximum_hz = 6000.0f;
        constexpr float bin_hz = static_cast<float>(kOutputSampleRate) / kFftN;
        for (size_t band = 0; band < MediaService::kSpectrumBandCount; ++band) {
            const float position =
                (static_cast<float>(band) + 0.5f) /
                MediaService::kSpectrumBandCount;
            const float center_hz = minimum_hz *
                std::pow(maximum_hz / minimum_hz, position);
            const int bin = std::clamp(
                static_cast<int>(center_hz / bin_hz + 0.5f), 1,
                kFftN / 2 - 1);
            coefficients[band] =
                2.0f * std::cos(2.0f * kPi * bin / kFftN);
        }

        float smooth[MediaService::kSpectrumBandCount]{};
        int16_t pcm[kFftN];
        while (spectrum_run.load(std::memory_order_relaxed)) {
            if (!want_play.load(std::memory_order_relaxed) ||
                shutdown.load(std::memory_order_relaxed)) {
                for (float& value : smooth) value *= (1.0f - kRelease);
                PublishSpectrum(smooth);
                vTaskDelay(pdMS_TO_TICKS(40));
                continue;
            }
            if (!PopPcm(pcm, kFftN)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            float raw[MediaService::kSpectrumBandCount];
            float peak = 1.0f;
            for (size_t band = 0; band < MediaService::kSpectrumBandCount;
                 ++band) {
                float previous = 0.0f;
                float previous2 = 0.0f;
                for (int index = 0; index < kFftN; ++index) {
                    const float current = pcm[index] * window[index] +
                                          coefficients[band] * previous -
                                          previous2;
                    previous2 = previous;
                    previous = current;
                }
                const float power = previous * previous +
                                    previous2 * previous2 -
                                    coefficients[band] * previous * previous2;
                raw[band] = std::sqrt(std::max(0.0f, power));
                peak = std::max(peak, raw[band]);
            }
            for (size_t band = 0; band < MediaService::kSpectrumBandCount;
                 ++band) {
                const float normalized =
                    std::sqrt(std::clamp(raw[band] / peak, 0.0f, 1.0f));
                const float factor = normalized > smooth[band] ? kAttack : kRelease;
                smooth[band] += (normalized - smooth[band]) * factor;
            }
            PublishSpectrum(smooth);
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        ClearSpectrum();
        spectrum_task = nullptr;
        vTaskDelete(nullptr);
    }

    void StartSpectrum() {
        if (spectrum_task != nullptr) return;
        pcm_write.store(0, std::memory_order_relaxed);
        pcm_read.store(0, std::memory_order_relaxed);
        ClearSpectrum();
        spectrum_run.store(true, std::memory_order_release);
        if (xTaskCreate(SpectrumTaskEntry, "external_fft", 6144, this, 4,
                        &spectrum_task) != pdPASS) {
            spectrum_run.store(false, std::memory_order_relaxed);
            spectrum_task = nullptr;
            ESP_LOGW(kTag, "failed to create spectrum task");
        }
    }

    void WaitTaskGone(TaskHandle_t* task, int maximum_ms) {
        const int steps = std::max(1, maximum_ms / 20);
        for (int index = 0; index < steps && *task != nullptr; ++index) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    void StopSpectrum() {
        spectrum_run.store(false, std::memory_order_release);
        WaitTaskGone(&spectrum_task, 1500);
        if (spectrum_task != nullptr) {
            ESP_LOGW(kTag, "spectrum task did not exit before timeout");
            spectrum_task = nullptr;
        }
        ClearSpectrum();
    }

    void EnsureMediaAdapter() {
        if (!media_adapter_ready) {
            media_lib_add_default_adapter();
            media_adapter_ready = true;
        }
    }

    static esp_gmf_err_t HttpScore(esp_gmf_io_handle_t, const char* url,
                                   int* score) {
        if (score == nullptr) return ESP_GMF_ERR_OK;
        *score = ESP_GMF_IO_SCORE_NONE;
        if (url == nullptr) return ESP_GMF_ERR_OK;
        const char* name = strrchr(url, '/');
        name = name != nullptr ? name + 1 : url;
        if (strstr(name, ".m3u8") != nullptr ||
            strstr(name, ".M3U8") != nullptr) {
            return ESP_GMF_ERR_OK;
        }
        if (strncasecmp(url, "http://", 7) == 0 ||
            strncasecmp(url, "https://", 8) == 0) {
            *score = ESP_GMF_IO_SCORE_STANDARD + 10;
        }
        return ESP_GMF_ERR_OK;
    }

    bool RegisterHttp() {
        http_io_cfg_t config = HTTP_STREAM_CFG_DEFAULT();
        config.dir = ESP_GMF_IO_DIR_READER;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        esp_gmf_io_handle_t io = nullptr;
        if (esp_gmf_io_http_init(&config, &io) != ESP_GMF_ERR_OK ||
            io == nullptr) {
            return false;
        }
        reinterpret_cast<esp_gmf_io_t*>(io)->get_score = HttpScore;
        if (esp_audio_simple_player_register_io(player, io) != ESP_GMF_ERR_OK) {
            esp_gmf_obj_delete(io);
            return false;
        }
        return true;
    }

    void ForceRateConversion(esp_gmf_pool_handle_t pool) {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
        if (pool == nullptr) return;
        const void* iterator = nullptr;
        esp_gmf_element_handle_t element = nullptr;
        while (esp_gmf_pool_iterate_element(pool, &iterator, &element) ==
               ESP_GMF_ERR_OK) {
            if (element == nullptr) continue;
            const char* tag = OBJ_GET_TAG(element);
            if (tag == nullptr || strcmp(tag, "aud_rate_cvt") != 0) continue;
            esp_gmf_rate_cvt_set_dest_rate(element, kOutputSampleRate);
            auto* config =
                static_cast<esp_ae_rate_cvt_cfg_t*>(OBJ_GET_CFG(element));
            if (config != nullptr) config->dest_rate = kOutputSampleRate;
        }
#else
        (void)pool;
#endif
    }

    void ApplyRateConversion() {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
        esp_gmf_pipeline_handle_t pipeline = nullptr;
        esp_gmf_element_handle_t element = nullptr;
        if (player == nullptr ||
            esp_audio_simple_player_get_pipeline(player, &pipeline) !=
                ESP_GMF_ERR_OK ||
            pipeline == nullptr) {
            return;
        }
        if (esp_gmf_pipeline_get_el_by_name(pipeline, "aud_rate_cvt", &element) ==
                ESP_GMF_ERR_OK &&
            element != nullptr) {
            esp_gmf_rate_cvt_set_dest_rate(element, kOutputSampleRate);
        }
#endif
    }

    static int OutputCallback(uint8_t* data, int data_size, void* context) {
        auto* self = static_cast<Impl*>(context);
        if (self == nullptr || data == nullptr || data_size <= 0 ||
            self->codec == nullptr ||
            !self->want_play.load(std::memory_order_relaxed) ||
            self->shutdown.load(std::memory_order_relaxed)) {
            return 0;
        }
        const auto* pcm = reinterpret_cast<const int16_t*>(data);
        const int sample_count = data_size / static_cast<int>(sizeof(int16_t));
        const int channels = self->pcm_channels.load(std::memory_order_relaxed);
        if (channels >= 2) {
            const int frames = sample_count / channels;
            self->pcm_buffer.resize(static_cast<size_t>(frames));
            for (int frame = 0; frame < frames; ++frame) {
                int sum = 0;
                for (int channel = 0; channel < channels; ++channel) {
                    sum += pcm[frame * channels + channel];
                }
                self->pcm_buffer[frame] =
                    static_cast<int16_t>(sum / channels);
            }
        } else {
            self->pcm_buffer.assign(pcm, pcm + sample_count);
        }
        self->PushPcm(self->pcm_buffer.data(),
                      static_cast<int>(self->pcm_buffer.size()));
        self->reported_playing.store(true, std::memory_order_release);
        self->public_state.store(MediaState::Playing,
                                 std::memory_order_release);
        self->codec->OutputData(self->pcm_buffer);
        return data_size;
    }

    static int PreviousCallback(esp_asp_handle_t*, void* context) {
        auto* self = static_cast<Impl*>(context);
        if (self != nullptr) self->ApplyRateConversion();
        return 0;
    }

    static int EventCallback(esp_asp_event_pkt_t* event, void* context) {
        auto* self = static_cast<Impl*>(context);
        if (self == nullptr || event == nullptr) return 0;
        if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
            event->payload_size >=
                static_cast<int>(sizeof(esp_asp_music_info_t))) {
            esp_asp_music_info_t info{};
            std::memcpy(&info, event->payload, sizeof(info));
            if (info.channels > 0) {
                self->pcm_channels.store(info.channels,
                                         std::memory_order_relaxed);
            }
        } else if (event->type == ESP_ASP_EVENT_TYPE_STATE &&
                   event->payload_size == sizeof(esp_asp_state_t)) {
            esp_asp_state_t state = ESP_ASP_STATE_NONE;
            std::memcpy(&state, event->payload, sizeof(state));
            if (state == ESP_ASP_STATE_RUNNING &&
                self->want_play.load(std::memory_order_relaxed)) {
                self->public_state.store(MediaState::Playing,
                                         std::memory_order_release);
            } else if (state == ESP_ASP_STATE_ERROR) {
                self->public_state.store(MediaState::Error,
                                         std::memory_order_release);
            }
        }
        return 0;
    }

    static int HlsMediaTypeCallback(esp_hls_file_seg_info_t* info,
                                    void* context) {
        auto* self = static_cast<Impl*>(context);
        if (self == nullptr || info == nullptr || self->player == nullptr ||
            self->hls_last_format == info->format) {
            return 0;
        }
        self->hls_last_format = info->format;
        esp_gmf_pipeline_handle_t pipeline = nullptr;
        esp_gmf_element_handle_t decoder = nullptr;
        if (esp_audio_simple_player_get_pipeline(self->player, &pipeline) !=
                ESP_GMF_ERR_OK ||
            pipeline == nullptr ||
            esp_gmf_pipeline_get_el_by_name(pipeline, "aud_dec", &decoder) !=
                ESP_GMF_ERR_OK ||
            decoder == nullptr) {
            return 0;
        }
        esp_gmf_info_sound_t sound{};
        sound.format_id = info->format;
        sound.sample_rates = 44100;
        sound.channels = 2;
        sound.bits = 16;
        esp_gmf_audio_dec_reconfig_by_sound_info(decoder, &sound);
        self->ApplyRateConversion();
        return 0;
    }

    void DestroyPlayer() {
        if (player == nullptr) return;
        esp_asp_handle_t old = player;
        player = nullptr;
        esp_audio_simple_player_stop(old);
        esp_audio_simple_player_destroy(old);
    }

    bool CreatePlayer() {
        DestroyPlayer();
        if (codec == nullptr) return false;
        hls_last_format = 0;
        pcm_channels.store(2, std::memory_order_relaxed);
        esp_asp_cfg_t config = {
            .in = {},
            .out = {.cb = OutputCallback, .user_ctx = this},
            .task_prio = 5,
            .task_stack = 8 * 1024,
            .prev = PreviousCallback,
            .prev_ctx = this,
        };
        if (esp_audio_simple_player_new(&config, &player) != ESP_GMF_ERR_OK ||
            player == nullptr) {
            return false;
        }
        esp_gmf_pool_handle_t pool = nullptr;
        if (esp_audio_simple_player_get_pool(player, &pool) != ESP_GMF_ERR_OK ||
            pool == nullptr) {
            DestroyPlayer();
            return false;
        }
        if (!RegisterHttp()) {
            DestroyPlayer();
            return false;
        }
        ForceRateConversion(pool);
        esp_gmf_io_handle_t hls = nullptr;
        esp_hls_io_cfg_t hls_config{};
        hls_config.name = "io_hls";
        hls_config.file_seg_cb = HlsMediaTypeCallback;
        hls_config.ctx = this;
        hls_config.pool = pool;
        hls_config.io_cfg = DEFAULT_HLS_IO_CFG();
        if (esp_gmf_io_hls_init(&hls_config, &hls) != ESP_GMF_ERR_OK ||
            hls == nullptr ||
            esp_audio_simple_player_register_io(player, hls) !=
                ESP_GMF_ERR_OK) {
            if (hls != nullptr) esp_gmf_obj_delete(hls);
            DestroyPlayer();
            return false;
        }
        esp_audio_simple_player_set_event(player, EventCallback, this);
        codec->EnableOutput(true);
        return true;
    }

    static void PlayerStopTask(void* argument) {
        auto* self = static_cast<Impl*>(argument);
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            esp_asp_handle_t active = self->player;
            if (active != nullptr) esp_audio_simple_player_stop(active);
            self->stop_pending.store(false, std::memory_order_release);
        }
    }

    void RequestPlayerStop() {
        bool expected = false;
        if (!stop_pending.compare_exchange_strong(expected, true)) return;
        if (player_stop_task == nullptr) {
            stop_pending.store(false, std::memory_order_release);
            ESP_LOGE(kTag, "media player-stop task was not reserved");
            public_state.store(MediaState::Error, std::memory_order_release);
            return;
        }
        xTaskNotifyGive(player_stop_task);
    }

    static void PlayTaskEntry(void* argument) {
        static_cast<Impl*>(argument)->PlayTaskMain();
    }

    void PlayTaskMain() {
        const TaskHandle_t self_task = xTaskGetCurrentTaskHandle();
        const uint32_t task_generation =
            generation.load(std::memory_order_acquire);
        EnsureMediaAdapter();
        if (!CreatePlayer()) {
            public_state.store(MediaState::Error, std::memory_order_release);
            if (play_task == self_task) play_task = nullptr;
            if (Lock()) {
                shutdown.store(true, std::memory_order_relaxed);
                want_play.store(false, std::memory_order_relaxed);
                suspend_after_stop = false;
                resume_after_suspend = false;
                life.store(LifeState::Stopping, std::memory_order_release);
                Unlock();
            }
            KickStopTask();
            vTaskDelete(nullptr);
            return;
        }
        while (!shutdown.load(std::memory_order_relaxed) &&
               generation.load(std::memory_order_relaxed) == task_generation) {
            if (!want_play.load(std::memory_order_relaxed)) {
                vTaskDelay(pdMS_TO_TICKS(40));
                continue;
            }
            std::string current_url;
            if (Lock()) {
                current_url = url;
                Unlock();
            }
            if (current_url.empty()) {
                public_state.store(MediaState::Error,
                                   std::memory_order_release);
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            reported_playing.store(false, std::memory_order_relaxed);
            public_state.store(MediaState::Connecting,
                               std::memory_order_release);
            ESP_LOGI(kTag, "play %s", current_url.c_str());
            const esp_gmf_err_t result = esp_audio_simple_player_run_to_end(
                player, current_url.c_str(), nullptr);
            if (shutdown.load(std::memory_order_relaxed) ||
                generation.load(std::memory_order_relaxed) != task_generation) {
                break;
            }
            if (!want_play.load(std::memory_order_relaxed)) continue;
            if (result != ESP_GMF_ERR_OK) {
                ESP_LOGW(kTag, "stream ended with 0x%x", result);
                public_state.store(MediaState::Error,
                                   std::memory_order_release);
                vTaskDelay(pdMS_TO_TICKS(1200));
            }
        }
        if (play_task == self_task) play_task = nullptr;
        vTaskDelete(nullptr);
    }

    static void StartTaskEntry(void* argument) {
        static_cast<Impl*>(argument)->StartTaskMain();
    }

    void StartTaskMain() {
        const TaskHandle_t self_task = xTaskGetCurrentTaskHandle();
        codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            public_state.store(MediaState::Error, std::memory_order_release);
            RestoreHlsLogLevels();
            life.store(LifeState::Idle, std::memory_order_release);
            if (start_task == self_task) start_task = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        Application::GetInstance().StopSystemAudioForStressTest();
        audio_acquired = true;
        if (shutdown.load(std::memory_order_relaxed) ||
            life.load(std::memory_order_relaxed) == LifeState::Stopping) {
            Application::GetInstance().RestoreSystemAudioAfterStressTest();
            audio_acquired = false;
            if (start_task == self_task) start_task = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        Application::GetInstance().GetAudioService()
            .SetExternalPlaybackActive(true);
        const uint32_t next_generation =
            generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        (void)next_generation;
        shutdown.store(false, std::memory_order_relaxed);
        want_play.store(true, std::memory_order_relaxed);
        stop_pending.store(false, std::memory_order_relaxed);
        reported_playing.store(false, std::memory_order_relaxed);
        public_state.store(MediaState::Connecting, std::memory_order_release);
        StartSpectrum();
        if (xTaskCreate(PlayTaskEntry, "external_hls", 8192, this, 5,
                        &play_task) != pdPASS) {
            play_task = nullptr;
            StopSpectrum();
            Application::GetInstance().GetAudioService()
                .SetExternalPlaybackActive(false);
            Application::GetInstance().RestoreSystemAudioAfterStressTest();
            audio_acquired = false;
            want_play.store(false, std::memory_order_relaxed);
            public_state.store(MediaState::Error, std::memory_order_release);
            RestoreHlsLogLevels();
            life.store(LifeState::Idle, std::memory_order_release);
            if (start_task == self_task) start_task = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        life.store(LifeState::Running, std::memory_order_release);
        if (start_task == self_task) start_task = nullptr;
        vTaskDelete(nullptr);
    }

    bool BeginStart(void* requested_owner) {
        if (!Lock()) return false;
        if (!OwnerMatches(requested_owner) ||
            life.load(std::memory_order_relaxed) == LifeState::Stopping) {
            Unlock();
            return false;
        }
        suspend_after_stop = false;
        resume_after_suspend = false;
        resume_requested = false;
        LimitHlsLogsToErrors();
        shutdown.store(false, std::memory_order_relaxed);
        want_play.store(true, std::memory_order_relaxed);
        public_state.store(MediaState::Connecting, std::memory_order_release);
        life.store(LifeState::Starting, std::memory_order_release);
        Unlock();
        if (xTaskCreate(StartTaskEntry, "external_media_on", 6144, this, 5,
                        &start_task) != pdPASS) {
            start_task = nullptr;
            life.store(LifeState::Idle, std::memory_order_release);
            public_state.store(MediaState::Error, std::memory_order_release);
            RestoreHlsLogLevels();
            return false;
        }
        return true;
    }

    static void StopTaskEntry(void* argument) {
        auto* self = static_cast<Impl*>(argument);
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            self->StopTaskMain();
        }
    }

    bool EnsureStopTask() {
        // Reserve both workers before GMF/HLS allocates its buffers. Creating
        // a one-shot stop task while switching stations can fail under media
        // pressure; falling back to esp_audio_simple_player_stop() on the UI
        // thread then freezes rendering until the network pipeline exits.
        if (stop_task == nullptr &&
            xTaskCreateWithCaps(
                StopTaskEntry, "external_media_off", 6144, this, 6,
                &stop_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            stop_task = nullptr;
            ESP_LOGE(kTag,
                     "failed to reserve media teardown task (internal=%u, psram=%u)",
                     static_cast<unsigned>(
                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(
                         heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            return false;
        }
        if (player_stop_task == nullptr &&
            xTaskCreateWithCaps(
                PlayerStopTask, "external_media_switch", 4096, this, 6,
                &player_stop_task,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            player_stop_task = nullptr;
            ESP_LOGE(kTag,
                     "failed to reserve media switch task (internal=%u, psram=%u)",
                     static_cast<unsigned>(
                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(
                         heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            return false;
        }
        return true;
    }

    void StopTaskMain() {
        want_play.store(false, std::memory_order_relaxed);
        shutdown.store(true, std::memory_order_relaxed);
        generation.fetch_add(1, std::memory_order_acq_rel);
        WaitTaskGone(&start_task, 4000);
        if (start_task != nullptr) {
            ESP_LOGW(kTag, "start task did not exit before timeout");
            start_task = nullptr;
        }
        // Abort the network pipeline before waiting for visualization cleanup;
        // otherwise HLS can fetch another segment for up to 1.5 seconds after
        // the App has already disappeared from the screen.
        const bool switch_stop_in_flight =
            stop_pending.load(std::memory_order_acquire);
        for (int index = 0; index < 50 &&
             stop_pending.load(std::memory_order_acquire); ++index) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        // Teardown already runs off the UI thread, so a timeout may safely use
        // the synchronous stop as the final cleanup fallback.
        if (player != nullptr &&
            (!switch_stop_in_flight ||
             stop_pending.load(std::memory_order_acquire))) {
            esp_audio_simple_player_stop(player);
        }
        StopSpectrum();
        stop_pending.store(false, std::memory_order_release);
        WaitTaskGone(&play_task, 4000);
        if (play_task != nullptr) {
            ESP_LOGW(kTag, "play task did not exit before timeout");
            play_task = nullptr;
        }
        DestroyPlayer();
        pcm_buffer.clear();
        pcm_buffer.shrink_to_fit();
        codec = nullptr;
        if (audio_acquired) {
            Application::GetInstance().GetAudioService()
                .SetExternalPlaybackActive(false);
            Application::GetInstance().RestoreSystemAudioAfterStressTest();
            audio_acquired = false;
        }

        bool suspended = false;
        bool restart_after_stop = false;
        void* restart_owner = nullptr;
        if (Lock(pdMS_TO_TICKS(2000))) {
            suspended = suspend_after_stop;
            suspend_after_stop = false;
            if (suspended) {
                restart_after_stop = resume_requested && resume_after_suspend;
                restart_owner = owner.load(std::memory_order_acquire);
                resume_requested = false;
                life.store(LifeState::Suspended, std::memory_order_release);
                public_state.store(MediaState::Paused,
                                   std::memory_order_release);
            } else {
                restart_owner =
                    pending_owner.exchange(nullptr, std::memory_order_acq_rel);
                restart_after_stop = restart_owner != nullptr;
                if (restart_after_stop) {
                    owner.store(restart_owner, std::memory_order_release);
                    url = std::move(pending_url);
                    life.store(LifeState::Idle, std::memory_order_release);
                    public_state.store(MediaState::Connecting,
                                       std::memory_order_release);
                } else {
                    life.store(LifeState::Idle, std::memory_order_release);
                    public_state.store(MediaState::Idle,
                                       std::memory_order_release);
                    owner.store(nullptr, std::memory_order_release);
                    url.clear();
                }
                pending_url.clear();
                resume_after_suspend = false;
                resume_requested = false;
            }
            if (!restart_after_stop) RestoreHlsLogLevels();
            Unlock();
        } else {
            life.store(LifeState::Idle, std::memory_order_release);
            public_state.store(MediaState::Idle, std::memory_order_release);
            owner.store(nullptr, std::memory_order_release);
            pending_owner.store(nullptr, std::memory_order_release);
            pending_url.clear();
            RestoreHlsLogLevels();
        }
        stop_worker_busy.store(false, std::memory_order_release);
        ESP_LOGI(kTag, "media cleanup complete");
        if (restart_after_stop && restart_owner != nullptr) {
            (void)BeginStart(restart_owner);
        }
    }

    void KickStopTask() {
        bool expected = false;
        if (!stop_worker_busy.compare_exchange_strong(expected, true)) return;
        if (stop_task == nullptr) {
            stop_worker_busy.store(false, std::memory_order_release);
            ESP_LOGE(kTag, "media stop task was not reserved");
            RequestPlayerStop();
            return;
        }
        xTaskNotifyGive(stop_task);
    }

    int BeginStop(void* requested_owner, bool suspend) {
        if (!Lock()) return -1;
        if (pending_owner.load(std::memory_order_acquire) == requested_owner) {
            pending_owner.store(nullptr, std::memory_order_release);
            pending_url.clear();
            Unlock();
            return 0;
        }
        if (!OwnerMatches(requested_owner)) {
            Unlock();
            return -1;
        }
        const LifeState current = life.load(std::memory_order_relaxed);
        if (current == LifeState::Idle || current == LifeState::Suspended) {
            if (suspend) {
                life.store(LifeState::Suspended, std::memory_order_release);
                public_state.store(MediaState::Paused,
                                   std::memory_order_release);
            } else {
                owner.store(nullptr, std::memory_order_release);
                url.clear();
                resume_after_suspend = false;
                resume_requested = false;
                life.store(LifeState::Idle, std::memory_order_release);
                public_state.store(MediaState::Idle,
                                   std::memory_order_release);
            }
            Unlock();
            return 0;
        }
        if (current == LifeState::Stopping) {
            if (!suspend) {
                suspend_after_stop = false;
                resume_after_suspend = false;
                resume_requested = false;
            }
            Unlock();
            return 0;
        }
        if (suspend) {
            resume_after_suspend = want_play.load(std::memory_order_relaxed);
            resume_requested = false;
        } else {
            resume_after_suspend = false;
            resume_requested = false;
        }
        suspend_after_stop = suspend;
        want_play.store(false, std::memory_order_relaxed);
        shutdown.store(true, std::memory_order_relaxed);
        life.store(LifeState::Stopping, std::memory_order_release);
        public_state.store(suspend ? MediaState::Paused : MediaState::Idle,
                           std::memory_order_release);
        Unlock();
        KickStopTask();
        return 0;
    }
};

MediaService::MediaService() : impl_(new Impl()) {
    s_existing_service = this;
    impl_->mutex = xSemaphoreCreateMutex();
}

MediaService& MediaService::Get() {
    static MediaService service;
    return service;
}

MediaService* MediaService::Existing() {
    return s_existing_service;
}

int MediaService::Start(void* owner, const char* url) {
    if (impl_ == nullptr || owner == nullptr || !IsSupportedUrl(url) ||
        !impl_->EnsureStopTask() || !impl_->Lock()) {
        return -1;
    }
    const LifeState current = impl_->life.load(std::memory_order_relaxed);
    const void* active_owner =
        impl_->owner.load(std::memory_order_acquire);
    if (current == LifeState::Stopping) {
        if (impl_->suspend_after_stop) {
            impl_->Unlock();
            return -2;
        }
        // Queue a successor even when the allocator reused the same State
        // address for the newly opened App.
        impl_->pending_owner.store(owner, std::memory_order_release);
        impl_->pending_url = url;
        impl_->Unlock();
        return 0;
    }
    if (active_owner != nullptr && active_owner != owner) {
        impl_->Unlock();
        return -2;
    }
    impl_->owner.store(owner, std::memory_order_release);
    impl_->url = url;
    if (current == LifeState::Running || current == LifeState::Starting) {
        impl_->want_play.store(true, std::memory_order_relaxed);
        impl_->shutdown.store(false, std::memory_order_relaxed);
        impl_->public_state.store(MediaState::Connecting,
                                  std::memory_order_release);
        impl_->Unlock();
        impl_->RequestPlayerStop();
        return 0;
    }
    impl_->Unlock();
    return impl_->BeginStart(owner) ? 0 : -1;
}

int MediaService::Pause(void* owner) {
    if (impl_ == nullptr || !impl_->Lock()) return -1;
    if (!impl_->OwnerMatches(owner) ||
        impl_->life.load(std::memory_order_relaxed) != LifeState::Running) {
        impl_->Unlock();
        return -1;
    }
    impl_->want_play.store(false, std::memory_order_relaxed);
    impl_->public_state.store(MediaState::Paused, std::memory_order_release);
    impl_->Unlock();
    impl_->RequestPlayerStop();
    return 0;
}

int MediaService::Resume(void* owner) {
    if (impl_ == nullptr || !impl_->Lock()) return -1;
    const LifeState current = impl_->life.load(std::memory_order_relaxed);
    if (!impl_->OwnerMatches(owner)) {
        impl_->Unlock();
        return -1;
    }
    if (current == LifeState::Suspended) {
        impl_->Unlock();
        return impl_->BeginStart(owner) ? 0 : -1;
    }
    if (current != LifeState::Running) {
        impl_->Unlock();
        return -1;
    }
    impl_->want_play.store(true, std::memory_order_relaxed);
    impl_->shutdown.store(false, std::memory_order_relaxed);
    impl_->public_state.store(MediaState::Connecting,
                              std::memory_order_release);
    impl_->Unlock();
    return 0;
}

int MediaService::Stop(void* owner) {
    return impl_ != nullptr ? impl_->BeginStop(owner, false) : -1;
}

MediaState MediaService::state(void* owner) const {
    if (impl_ == nullptr || owner == nullptr) {
        return MediaState::Idle;
    }
    if (impl_->pending_owner.load(std::memory_order_acquire) == owner) {
        return MediaState::Connecting;
    }
    if (impl_->owner.load(std::memory_order_acquire) != owner) {
        return MediaState::Idle;
    }
    return impl_->public_state.load(std::memory_order_acquire);
}

int MediaService::GetSpectrum(void* owner, uint8_t* levels,
                              size_t count) const {
    if (impl_ == nullptr || owner == nullptr || levels == nullptr ||
        count < kSpectrumBandCount) {
        return -1;
    }
    if (impl_->pending_owner.load(std::memory_order_acquire) == owner) {
        std::memset(levels, 0, kSpectrumBandCount);
        return 0;
    }
    if (impl_->owner.load(std::memory_order_acquire) != owner) return -1;
    portENTER_CRITICAL(&impl_->spectrum_mux);
    std::memcpy(levels, impl_->spectrum_levels, kSpectrumBandCount);
    portEXIT_CRITICAL(&impl_->spectrum_mux);
    return 0;
}

int MediaService::GetVolume(void* owner) const {
    if (impl_ == nullptr || owner == nullptr) return -1;
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    return codec != nullptr ? std::clamp(codec->output_volume(), 0, 100) : -1;
}

int MediaService::SetVolume(void* owner, int volume) {
    if (impl_ == nullptr || owner == nullptr ||
        (impl_->owner.load(std::memory_order_acquire) != owner &&
         impl_->pending_owner.load(std::memory_order_acquire) != owner)) {
        return -1;
    }
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) return -1;
    codec->SetOutputVolume(std::clamp(volume, 0, 100));
    return 0;
}

void MediaService::SuspendOwner(void* owner) {
    if (impl_ != nullptr) (void)impl_->BeginStop(owner, true);
}

void MediaService::ResumeOwner(void* owner) {
    if (impl_ == nullptr || !impl_->Lock()) return;
    const LifeState current = impl_->life.load(std::memory_order_relaxed);
    const bool owner_matches = impl_->OwnerMatches(owner);
    const bool should_resume = owner_matches &&
                               current == LifeState::Suspended &&
                               impl_->resume_after_suspend;
    if (owner_matches && current == LifeState::Stopping &&
        impl_->suspend_after_stop && impl_->resume_after_suspend) {
        impl_->resume_requested = true;
    }
    impl_->Unlock();
    if (should_resume) (void)impl_->BeginStart(owner);
}

void MediaService::UnloadOwner(void* owner) {
    if (impl_ != nullptr) (void)impl_->BeginStop(owner, false);
}

bool MediaService::ResetForAppLaunch(uint32_t timeout_ms) {
    if (impl_ == nullptr) return true;

    void* active_owner = nullptr;
    if (impl_->Lock()) {
        // A queued successor belongs to a Runtime::State that may already have
        // been deleted. No successor may survive across the system launch
        // barrier.
        impl_->pending_owner.store(nullptr, std::memory_order_release);
        impl_->pending_url.clear();
        impl_->suspend_after_stop = false;
        impl_->resume_after_suspend = false;
        impl_->resume_requested = false;
        active_owner = impl_->owner.load(std::memory_order_acquire);
        impl_->Unlock();
    } else {
        return false;
    }
    if (active_owner != nullptr) {
        (void)impl_->BeginStop(active_owner, false);
    }

    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (impl_->life.load(std::memory_order_acquire) != LifeState::Idle ||
           impl_->stop_worker_busy.load(std::memory_order_acquire)) {
        if (static_cast<TickType_t>(xTaskGetTickCount() - started) >=
            timeout_ticks) {
            ESP_LOGE(kTag,
                     "media reset timed out (life=%u, busy=%u, internal=%u, "
                     "largest=%u)",
                     static_cast<unsigned>(
                         impl_->life.load(std::memory_order_relaxed)),
                     static_cast<unsigned>(impl_->stop_worker_busy.load(
                         std::memory_order_relaxed)),
                     static_cast<unsigned>(
                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(
                         MALLOC_CAP_INTERNAL)));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (!impl_->Lock()) return false;
    impl_->owner.store(nullptr, std::memory_order_release);
    impl_->pending_owner.store(nullptr, std::memory_order_release);
    impl_->url.clear();
    impl_->pending_url.clear();
    impl_->public_state.store(MediaState::Idle, std::memory_order_release);
    impl_->Unlock();
    return true;
}

}  // namespace agent_ui::external_apps
