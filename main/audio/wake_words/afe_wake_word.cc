#include "afe_wake_word.h"
#include "audio_service.h"

#include <esp_log.h>
#include <sstream>

#define DETECTION_RUNNING_EVENT BIT0
#define DETECTION_IDLE_EVENT BIT1

namespace {
constexpr TickType_t kFetchStopPollTicks = pdMS_TO_TICKS(100);
constexpr BaseType_t kAudioDetectionCore = 1;
}

#define TAG "AfeWakeWord"

AfeWakeWord::AfeWakeWord()
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {

    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }

    vEventGroupDelete(event_group_);
}

bool AfeWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    int ref_num = codec_->input_reference() ? 1 : 0;

    if (models_list == nullptr) {
        models_ = esp_srmodel_init("model");
    } else {
        models_ = models_list;
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }
    for (int i = 0; i < models_->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        if (strstr(models_->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model_ = models_->model_name[i];
            auto words = esp_srmodel_get_wake_words(models_, wakenet_model_);
            // split by ";" to get all wake words
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                wake_words_.push_back(word);
            }
        }
    }

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models_, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    
    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);

    BaseType_t task_created = xTaskCreatePinnedToCore([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        this_->AudioDetectionTask();
        vTaskDelete(NULL);
    }, "audio_detection", 4096, this, 3, nullptr, kAudioDetectionCore);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio detection task on core %d",
            static_cast<int>(kAudioDetectionCore));
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        return false;
    }

    return true;
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void AfeWakeWord::Start() {
    std::lock_guard<std::mutex> lock(detection_lifecycle_mutex_);
    if (afe_data_ == nullptr || detection_running_.load()) {
        return;
    }

    // A previous run may still be resetting the AFE after detecting a wake word.
    xEventGroupWaitBits(event_group_, DETECTION_IDLE_EVENT,
        pdFALSE, pdTRUE, portMAX_DELAY);
    detection_running_.store(true);
    xEventGroupClearBits(event_group_, DETECTION_IDLE_EVENT);
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop() {
    std::lock_guard<std::mutex> lock(detection_lifecycle_mutex_);
    if (afe_data_ == nullptr) {
        return;
    }

    detection_running_.store(false);
    // Wake a task that has not entered fetch yet, as well as one polling fetch.
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
    // AudioDetectionTask owns fetch/reset. Waiting for its idle acknowledgement
    // prevents reset_buffer() from racing fetch_with_delay() or the final Feed().
    xEventGroupWaitBits(event_group_, DETECTION_IDLE_EVENT,
        pdFALSE, pdTRUE, portMAX_DELAY);
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr || !detection_running_.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(afe_feed_mutex_);
    if (!detection_running_.load()) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

size_t AfeWakeWord::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::AudioDetectionTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio detection task started on core %d, feed size: %d fetch size: %d",
        xPortGetCoreID(), feed_size, fetch_size);
    xEventGroupSetBits(event_group_, DETECTION_IDLE_EVENT);

    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdTRUE, pdTRUE, portMAX_DELAY);
        if (!detection_running_.load()) {
            xEventGroupSetBits(event_group_, DETECTION_IDLE_EVENT);
            continue;
        }
        xEventGroupClearBits(event_group_, DETECTION_IDLE_EVENT);

        bool wake_word_detected = false;
        while (detection_running_.load()) {
            auto res = afe_iface_->fetch_with_delay(afe_data_, kFetchStopPollTicks);
            if (!detection_running_.load()) {
                break;
            }
            if (res == nullptr || res->ret_value == ESP_FAIL) {
                continue;
            }

            // Store the wake word data for voice recognition, like who is speaking
            StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

            if (res->wakeup_state == WAKENET_DETECTED) {
                detection_running_.store(false);
                last_detected_wake_word_ = wake_words_[res->wakenet_model_index - 1];
                wake_word_detected = true;
                break;
            }
        }

        // No new Feed() can start once detection_running_ is false. Taking the
        // feed lock waits for an already-running feed before resetting buffers.
        {
            std::lock_guard<std::mutex> lock(afe_feed_mutex_);
            afe_iface_->reset_buffer(afe_data_);
        }
        xEventGroupSetBits(event_group_, DETECTION_IDLE_EVENT);

        if (wake_word_detected && wake_word_detected_callback_) {
            wake_word_detected_callback_(last_detected_wake_word_);
        }
    }
}

void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    // store audio data to wake_word_pcm_
    wake_word_pcm_.emplace_back(std::vector<int16_t>(data, data + samples));
    // keep about 2 seconds of data, detect duration is 30ms (sample_rate == 16000, chunksize == 512)
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}

void AfeWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            auto encoder = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
            encoder->SetComplexity(0); // 0 is the fastest

            int packets = 0;
            for (auto& pcm: this_->wake_word_pcm_) {
                encoder->Encode(std::move(pcm), [this_](std::vector<uint8_t>&& opus) {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.emplace_back(std::move(opus));
                    this_->wake_word_cv_.notify_all();
                });
                packets++;
            }
            this_->wake_word_pcm_.clear();

            auto end_time = esp_timer_get_time();
            ESP_LOGD(TAG, "Encode wake word opus %d packets in %ld ms", packets, (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
