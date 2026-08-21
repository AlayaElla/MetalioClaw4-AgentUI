#include "audio_service.h"
#include <algorithm>
#include <cmath>
#include <esp_log.h>
#include <cstring>

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"
#else
#include "wake_words/esp_wake_word.h"
#endif

#define TAG "AudioService"


AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}


void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    voice_detected_.store(false, std::memory_order_release);
    listening_audio_enabled_.store(false, std::memory_order_release);
    listening_audio_noise_floor_reset_pending_.store(true,
                                                     std::memory_order_release);
    listening_audio_features_.Clear();
    codec_->Start();

    /* Setup the audio codec */
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
#if CONFIG_USE_AUDIO_PROCESSOR
        if (listening_audio_enabled_.load(std::memory_order_acquire)) {
            // AFE has already denoised/AEC'd this 60 ms frame.  Analyze it
            // before moving the same vector into the Opus queue.
            UpdateListeningAudioFeatures(data);
        }
#else
        listening_audio_features_.PublishActivity(0.0f, false);
#endif
        // This is the only point where AFE-processed 16 kHz mono frames are
        // available.  The Hermes recorder is strictly copy-only here.
        {
            std::lock_guard<std::mutex> lock(processed_pcm_callback_mutex_);
            if (processed_pcm_callback_) {
                processed_pcm_callback_(data.data(), data.size());
            }
        }
        {
            std::lock_guard<std::mutex> lock(
                external_recording_pcm_callback_mutex_);
            if (external_recording_pcm_callback_) {
                external_recording_pcm_callback_(data.data(), data.size());
            }
        }
        if (network_audio_enabled_.load(std::memory_order_acquire)) {
            PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
        }
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        const bool was_speaking =
            voice_detected_.exchange(speaking, std::memory_order_acq_rel);
        listening_audio_features_.SetSpeech(speaking);
        if (speaking && !was_speaking) {
            listening_audio_features_.MarkOnset();
        }
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_ALL_TASKS_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 4, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 13, this, 2, &opus_codec_task_handle_);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    listening_audio_enabled_.store(false, std::memory_order_release);
    voice_detected_.store(false, std::memory_order_release);
    listening_audio_noise_floor_reset_pending_.store(
        true, std::memory_order_release);
    listening_audio_features_.Clear();
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    network_audio_generation_.fetch_add(1, std::memory_order_acq_rel);
    audio_encode_queue_.clear();
    audio_send_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    pending_send_packets_ = 0;
    audio_queue_cv_.notify_all();
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (codec_->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    input_sequence_.fetch_add(1);
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    xEventGroupSetBits(event_group_, AS_EVENT_INPUT_TASK_RUNNING);
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word */
        if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
            std::vector<int16_t> data;
            int samples = wake_word_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    wake_word_->Feed(data);
                    continue;
                }
            }
        }

        /* Feed the audio processor */
        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
            std::vector<int16_t> data;
            int samples = audio_processor_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    audio_processor_->Feed(std::move(data));
                    continue;
                }
            }
        }

        // A route transition can temporarily remove the I2S input clock.
        // Keep this long-lived task alive so capture resumes with the route.
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    xEventGroupClearBits(event_group_, AS_EVENT_INPUT_TASK_RUNNING);
    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    xEventGroupSetBits(event_group_, AS_EVENT_OUTPUT_TASK_RUNNING);
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        if (service_stopped_) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        const bool is_pcm_playback = task->type == kAudioTaskTypePcmPlaybackQueue;
        if (is_pcm_playback) pcm_playback_active_.fetch_add(1, std::memory_order_acq_rel);
        audio_queue_cv_.notify_all();
        lock.unlock();

        std::unique_lock<std::mutex> pcm_output_lock(pcm_output_mutex_, std::defer_lock);
        if (is_pcm_playback) {
            pcm_output_lock.lock();
            if (task->pcm_playback_generation !=
                pcm_playback_generation_.load(std::memory_order_acquire)) {
                pcm_playback_active_.fetch_sub(1, std::memory_order_acq_rel);
                audio_queue_cv_.notify_all();
                continue;
            }
        }

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }
        codec_->OutputData(task->pcm);
        if (is_pcm_playback) {
            pcm_playback_active_.fetch_sub(1, std::memory_order_acq_rel);
            audio_queue_cv_.notify_all();
        }

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            lock.lock();
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
    }

    xEventGroupClearBits(event_group_, AS_EVENT_OUTPUT_TASK_RUNNING);
    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    xEventGroupSetBits(event_group_, AS_EVENT_CODEC_TASK_RUNNING);
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() &&
                 (audio_encode_queue_.front()->type != kAudioTaskTypeEncodeToSendQueue ||
                  audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE)) ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_->Decode(std::move(packet->payload), task->pcm)) {
                // Resample if the sample rate is different
                if (opus_decoder_->sample_rate() != codec_->output_sample_rate()) {
                    int target_size = output_resampler_.GetOutputSamples(task->pcm.size());
                    std::vector<int16_t> resampled(target_size);
                    output_resampler_.Process(task->pcm.data(), task->pcm.size(), resampled.data());
                    task->pcm = std::move(resampled);
                }

                lock.lock();
                audio_playback_queue_.push_back(std::move(task));
                audio_queue_cv_.notify_all();
            } else {
                ESP_LOGE(TAG, "Failed to decode audio");
                lock.lock();
            }
            debug_statistics_.decode_count++;
        }
        
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() &&
            (audio_encode_queue_.front()->type != kAudioTaskTypeEncodeToSendQueue ||
             audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE)) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;
            if (!opus_encoder_->Encode(std::move(task->pcm), packet->payload)) {
                ESP_LOGE(TAG, "Failed to encode audio");
                if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                    bool notify = false;
                    {
                        std::lock_guard<std::mutex> queue_lock(audio_queue_mutex_);
                        if (network_audio_enabled_.load(std::memory_order_acquire) &&
                            task->network_audio_generation ==
                                network_audio_generation_.load(std::memory_order_acquire)) {
                            pending_send_packets_.fetch_sub(1);
                            notify = true;
                        }
                    }
                    if (notify && callbacks_.on_send_queue_available) {
                        callbacks_.on_send_queue_available();
                    }
                }
                if (task->on_encoded) {
                    task->on_encoded(nullptr);
                }
                continue;
            }

            if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    if (network_audio_enabled_.load(std::memory_order_acquire) &&
                        task->network_audio_generation ==
                            network_audio_generation_.load(std::memory_order_acquire)) {
                        audio_send_queue_.push_back(std::move(packet));
                    }
                }
                if (packet == nullptr && callbacks_.on_send_queue_available) {
                    callbacks_.on_send_queue_available();
                }
            } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                audio_testing_queue_.push_back(std::move(packet));
            } else if (task->type == kAudioTaskTypeEncodeForCallback && task->on_encoded) {
                task->on_encoded(std::move(packet));
            }
            debug_statistics_.encode_count++;
            lock.lock();
        }
    }

    xEventGroupClearBits(event_group_, AS_EVENT_CODEC_TASK_RUNNING);
    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    if (type == kAudioTaskTypeEncodeToSendQueue) {
        if (!network_audio_enabled_.load(std::memory_order_acquire)) return;
        task->network_audio_generation =
            network_audio_generation_.load(std::memory_order_acquire);
    }

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    if (type == kAudioTaskTypeEncodeToSendQueue) {
        pending_send_packets_.fetch_add(1);
    }

    audio_queue_cv_.wait(lock, [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

void AudioService::EncodeAudio(
        std::vector<int16_t>&& pcm,
        std::function<void(std::unique_ptr<AudioStreamPacket>)> callback) {
    if (!callback) {
        ESP_LOGE(TAG, "Missing audio encode callback");
        return;
    }
    auto task = std::make_unique<AudioTask>();
    task->type = kAudioTaskTypeEncodeForCallback;
    task->pcm = std::move(pcm);
    task->on_encoded = std::move(callback);

    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() {
        return service_stopped_ || audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE;
    });
    if (service_stopped_) {
        lock.unlock();
        task->on_encoded(nullptr);
        return;
    }
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

void AudioService::SetProcessedPcmCallback(
        std::function<void(const int16_t*, size_t)> callback) {
    std::lock_guard<std::mutex> lock(processed_pcm_callback_mutex_);
    processed_pcm_callback_ = std::move(callback);
}

void AudioService::SetExternalRecordingPcmCallback(
        std::function<void(const int16_t*, size_t)> callback) {
    std::lock_guard<std::mutex> lock(
        external_recording_pcm_callback_mutex_);
    external_recording_pcm_callback_ = std::move(callback);
}

void AudioService::SetExternalRecordingActive(bool active) {
    std::lock_guard<std::mutex> lock(input_route_mutex_);
    if (external_recording_active_ == active) return;
    external_recording_active_ = active;
    UpdateInputRoutesLocked();
}

void AudioService::SetExternalPlaybackActive(bool active) {
    external_playback_active_.store(active, std::memory_order_release);
    if (active && audio_power_timer_ != nullptr) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(
            audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
    }
}

void AudioService::SetNetworkAudioEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    network_audio_enabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        network_audio_generation_.fetch_add(1, std::memory_order_acq_rel);
        audio_encode_queue_.erase(std::remove_if(audio_encode_queue_.begin(),
            audio_encode_queue_.end(), [](const std::unique_ptr<AudioTask>& task) {
                return task->type == kAudioTaskTypeEncodeToSendQueue;
            }), audio_encode_queue_.end());
        audio_send_queue_.clear();
        pending_send_packets_.store(0, std::memory_order_release);
        audio_queue_cv_.notify_all();
    }
}

bool AudioService::PushPcmToPlaybackQueue(std::vector<int16_t>&& pcm,
                                          uint32_t playback_generation) {
    if (pcm.empty() || pcm.size() != OPUS_FRAME_DURATION_MS * 16000 / 1000) return false;
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (playback_generation != pcm_playback_generation_.load(std::memory_order_acquire) ||
        audio_playback_queue_.size() >= MAX_PLAYBACK_TASKS_IN_QUEUE) return false;
    auto task = std::make_unique<AudioTask>();
    task->type = kAudioTaskTypePcmPlaybackQueue;
    task->pcm_playback_generation = playback_generation;
    task->pcm = std::move(pcm);
    audio_playback_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
    return true;
}

bool AudioService::IsPcmPlaybackIdle() const {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return pcm_playback_active_.load(std::memory_order_acquire) == 0 &&
        std::none_of(audio_playback_queue_.begin(), audio_playback_queue_.end(),
        [](const std::unique_ptr<AudioTask>& task) {
            return task->type == kAudioTaskTypePcmPlaybackQueue;
        });
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            return false;
        }
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    pending_send_packets_.fetch_sub(1);
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    std::lock_guard<std::mutex> lock(input_route_mutex_);
    wake_word_requested_ = enable;
    UpdateInputRoutesLocked();
}

void AudioService::ApplyWakeWordDetectionLocked(bool enable) {
    if (!wake_word_) {
        return;
    }

    const bool currently_enabled =
        (xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING) != 0;
    if (enable == currently_enabled) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        wake_word_->Stop();
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    std::lock_guard<std::mutex> lock(input_route_mutex_);
    voice_processing_requested_ = enable;
    UpdateInputRoutesLocked();
}

void AudioService::ApplyVoiceProcessingLocked(bool enable) {
    const bool currently_enabled =
        (xEventGroupGetBits(event_group_) &
         AS_EVENT_AUDIO_PROCESSOR_RUNNING) != 0;
    if (enable == currently_enabled) return;
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        listening_audio_noise_floor_reset_pending_.store(
            true, std::memory_order_release);
        listening_audio_enabled_.store(true, std::memory_order_release);
        listening_audio_features_.Clear();
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        listening_audio_enabled_.store(false, std::memory_order_release);
        voice_detected_.store(false, std::memory_order_release);
        listening_audio_noise_floor_reset_pending_.store(
            true, std::memory_order_release);
        listening_audio_features_.Clear();
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::UpdateInputRoutesLocked() {
    const bool want_voice =
        voice_processing_requested_ || external_recording_active_;
    const bool want_wake = wake_word_requested_ && !want_voice;
    const EventBits_t bits = xEventGroupGetBits(event_group_);
    const bool wake_active = (bits & AS_EVENT_WAKE_WORD_RUNNING) != 0;
    const bool voice_active =
        (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) != 0;

    // Disable the old route first because the input task gives wake-word
    // capture priority over processed PCM when both bits are set.
    if (wake_active && !want_wake) ApplyWakeWordDetectionLocked(false);
    if (voice_active && !want_voice) ApplyVoiceProcessingLocked(false);
    if (!voice_active && want_voice) ApplyVoiceProcessingLocked(true);
    if (!wake_active && want_wake) ApplyWakeWordDetectionLocked(true);
}

void AudioService::UpdateListeningAudioFeatures(
    const std::vector<int16_t>& pcm) {
    if (listening_audio_noise_floor_reset_pending_.exchange(
            false, std::memory_order_acq_rel)) {
        listening_audio_noise_floor_ = 0.008f;
    }
    if (pcm.empty()) {
        listening_audio_features_.PublishActivity(
            0.0f, voice_detected_.load(std::memory_order_acquire));
        return;
    }

    double squared_sum = 0.0;
    for (const int16_t sample : pcm) {
        const float normalized = static_cast<float>(sample) / 32768.0f;
        squared_sum += static_cast<double>(normalized) * normalized;
    }
    const float rms = static_cast<float>(
        std::sqrt(squared_sum / static_cast<double>(pcm.size())));
    const bool speech = voice_detected_.load(std::memory_order_acquire);

    // Adapt only during silence or low-energy speech frames.  The asymmetric
    // gains make the floor rise slowly and fall even more slowly, so speech
    // does not become its own noise reference.
    constexpr float kNoiseFloorMin = 0.002f;
    constexpr float kNoiseFloorMax = 0.12f;
    constexpr float kLowEnergyMultiplier = 1.5f;
    constexpr float kNoiseRiseAlpha = 0.06f;   // one 60 ms frame
    constexpr float kNoiseFallAlpha = 0.012f;  // one 60 ms frame
    if (!speech || rms <= listening_audio_noise_floor_ * kLowEnergyMultiplier) {
        const float target = std::max(kNoiseFloorMin,
                                      std::min(kNoiseFloorMax, rms));
        const float alpha = target > listening_audio_noise_floor_
                                ? kNoiseRiseAlpha
                                : kNoiseFallAlpha;
        listening_audio_noise_floor_ +=
            (target - listening_audio_noise_floor_) * alpha;
        listening_audio_noise_floor_ = std::max(
            kNoiseFloorMin,
            std::min(kNoiseFloorMax, listening_audio_noise_floor_));
    }

    constexpr float kActivityMinimumRange = 0.035f;
    constexpr float kActivityFloorScale = 3.0f;
    constexpr float kActivityOffset = 0.012f;
    const float signal = std::max(0.0f, rms - listening_audio_noise_floor_);
    const float range = std::max(
        kActivityMinimumRange,
        listening_audio_noise_floor_ * kActivityFloorScale + kActivityOffset);
    const float activity = std::max(0.0f, std::min(1.0f, signal / range));
    listening_audio_features_.PublishActivity(activity, speech);
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& ogg) {
    if (ogg.empty()) {
        return;
    }

    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const uint8_t* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();
    size_t offset = 0;

    auto find_page = [&](size_t start)->size_t {
        for (size_t i = start; i + 4 <= size; ++i) {
            if (buf[i] == 'O' && buf[i+1] == 'g' && buf[i+2] == 'g' && buf[i+3] == 'S') return i;
        }
        return static_cast<size_t>(-1);
    };

    bool seen_head = false;
    bool seen_tags = false;
    int sample_rate = 16000; // 默认值

    while (true) {
        size_t pos = find_page(offset);
        if (pos == static_cast<size_t>(-1)) break;
        offset = pos;
        if (offset + 27 > size) break;

        const uint8_t* page = buf + offset;
        uint8_t page_segments = page[26];
        size_t seg_table_off = offset + 27;
        if (seg_table_off + page_segments > size) break;

        size_t body_size = 0;
        for (size_t i = 0; i < page_segments; ++i) body_size += page[27 + i];

        size_t body_off = seg_table_off + page_segments;
        if (body_off + body_size > size) break;

        // Parse packets using lacing
        size_t cur = body_off;
        size_t seg_idx = 0;
        while (seg_idx < page_segments) {
            size_t pkt_len = 0;
            size_t pkt_start = cur;
            bool continued = false;
            do {
                uint8_t l = page[27 + seg_idx++];
                pkt_len += l;
                cur += l;
                continued = (l == 255);
            } while (continued && seg_idx < page_segments);

            if (pkt_len == 0) continue;
            const uint8_t* pkt_ptr = buf + pkt_start;

            if (!seen_head) {
                // 解析OpusHead包
                if (pkt_len >= 19 && std::memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                    seen_head = true;
                    
                    // OpusHead结构：[0-7] "OpusHead", [8] version, [9] channel_count, [10-11] pre_skip
                    // [12-15] input_sample_rate, [16-17] output_gain, [18] mapping_family
                    if (pkt_len >= 12) {
                        uint8_t version = pkt_ptr[8];
                        uint8_t channel_count = pkt_ptr[9];
                        
                        if (pkt_len >= 16) {
                            // 读取输入采样率 (little-endian)
                            sample_rate = pkt_ptr[12] | (pkt_ptr[13] << 8) | 
                                        (pkt_ptr[14] << 16) | (pkt_ptr[15] << 24);
                            ESP_LOGD(TAG, "OpusHead: version=%d, channels=%d, sample_rate=%d", 
                                   version, channel_count, sample_rate);
                        }
                    }
                }
                continue;
            }
            if (!seen_tags) {
                // Expect OpusTags in second packet
                if (pkt_len >= 8 && std::memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                    seen_tags = true;
                }
                continue;
            }

            // Audio packet (Opus)
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            packet->payload.resize(pkt_len);
            std::memcpy(packet->payload.data(), pkt_ptr, pkt_len);
            PushPacketToDecodeQueue(std::move(packet), true);
        }

        offset = body_off + body_size;
    }
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

void AudioService::ResetDecoder() {
    // Serialize cancellation with the start of a PCM OutputData call. A frame
    // already being written may finish, but no stale frame can start after
    // this method returns.
    std::lock_guard<std::mutex> output_lock(pcm_output_mutex_);
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    pcm_playback_generation_.fetch_add(1, std::memory_order_acq_rel);
    opus_decoder_->ResetState();
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (!external_playback_active_.load(std::memory_order_acquire) &&
        output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    if (!codec_->input_enabled() && !codec_->output_enabled() &&
        !external_playback_active_.load(std::memory_order_acquire)) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#else
    if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
}

bool AudioService::IsAfeWakeWord() {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    return wake_word_ != nullptr && dynamic_cast<AfeWakeWord*>(wake_word_.get()) != nullptr;
#else
    return false;
#endif
}
