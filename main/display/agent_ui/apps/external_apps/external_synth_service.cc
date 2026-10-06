#include "external_synth_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <new>
#include <vector>

#include "external_synth_renderer.h"

#ifdef EXTERNAL_SYNTH_HOST_TEST
#include "external_synth_host_platform.h"
#else
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "esp_log.h"
#include "external_media_service.h"
#include "external_recording_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ai/ai_availability.h"
#endif

namespace agent_ui::external_apps {
namespace {

constexpr char kTag[] = "ExternalSynth";
constexpr uint32_t kSampleRate = SynthRenderer::kSampleRate;
constexpr uint32_t kFramesPerBlock = 160;
constexpr uint32_t kBlockMs = 10;

std::atomic<SynthService*> s_existing_service{nullptr};

bool ValidParams(const metalio_app_synth_params_t* params) {
    return params != nullptr &&
           params->waveform <= METALIO_APP_SYNTH_THEREMIN;
}

metalio_app_synth_params_t DefaultParams() {
    metalio_app_synth_params_t params{};
    params.frequency_millihz = 440000;
    params.level_per_mille = 0;
    params.waveform = METALIO_APP_SYNTH_SINE;
    params.level_ramp_ms = 5;
    params.brightness_per_mille = 1000;
    return params;
}

}  // namespace

struct SynthService::Impl {
    struct Snapshot {
        void* owner = nullptr;
        metalio_app_synth_params_t params{};
        bool params_valid = false;
        bool params_dirty = false;
        bool start_requested = false;
        bool release_requested = false;
        bool interrupted = false;
    };

    portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
    void* owner = nullptr;
    metalio_app_synth_params_t params = DefaultParams();
    bool params_valid = false;
    bool params_dirty = false;
    bool start_requested = false;
    bool release_requested = false;
    bool interrupted = false;
    std::atomic<bool> active{false};
    std::atomic<bool> releasing{false};
    std::atomic<metalio_app_synth_state_t> public_state{
        METALIO_APP_SYNTH_IDLE};
    std::atomic<bool> worker_ready{false};
    TaskHandle_t worker = nullptr;
    AudioCodec* codec = nullptr;
    bool focus_acquired = false;  // Worker-only.
    bool release_in_progress = false; // Worker-only.
    uint32_t release_samples = 0; // Worker-only.
    SynthRenderer renderer;      // Worker-only.
    std::vector<int16_t> pcm = std::vector<int16_t>(kFramesPerBlock);
    std::mutex callback_mutex;
    std::function<void(bool)> assistant_callback;

    Impl() {
        if (xTaskCreate(WorkerEntry, "external_synth", 4096, this, 5,
                        &worker) != pdPASS) {
            worker = nullptr;
            ESP_LOGE(kTag, "failed to reserve synthesizer worker");
            return;
        }
        worker_ready.store(true, std::memory_order_release);
    }

    Snapshot CopySnapshot() {
        Snapshot snapshot;
        portENTER_CRITICAL(&state_mux);
        snapshot.owner = owner;
        snapshot.params = params;
        snapshot.params_valid = params_valid;
        snapshot.params_dirty = params_dirty;
        snapshot.start_requested = start_requested;
        snapshot.release_requested = release_requested;
        snapshot.interrupted = interrupted;
        params_dirty = false;
        start_requested = false;
        portEXIT_CRITICAL(&state_mux);
        return snapshot;
    }

    void Notify() {
        if (worker != nullptr) xTaskNotifyGive(worker);
    }

    bool RouteStillWanted() {
        portENTER_CRITICAL(&state_mux);
        const bool wanted = active.load(std::memory_order_relaxed) &&
                            !release_requested && owner != nullptr;
        portEXIT_CRITICAL(&state_mux);
        return wanted;
    }

    bool CanAcquire() {
        Application& app = Application::GetInstance();
        RecordingService* recording = RecordingService::Existing();
        return app.GetDeviceState() == kDeviceStateIdle &&
               !app.IsLowPowerStandby() &&
               !app.IsCodexVoiceCaptureActive() &&
               !app.IsCodexRealtimePlaybackActive() &&
               ai::Availability::Get().IsAvailable() &&
               (recording == nullptr || !recording->IsActive());
    }

    void CompleteAssistantCallback(bool ready) {
        std::function<void(bool)> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = std::move(assistant_callback);
        }
        if (callback) callback(ready);
    }

    void RestoreRoute() {
        if (!focus_acquired) return;
        Application& app = Application::GetInstance();
        // A protocol takeover may arrive while the fade is in progress. Clear
        // our playback lease, but do not disable a codec output that a newer
        // system owner has already started using.
        const bool system_claimed =
            app.GetDeviceState() != kDeviceStateIdle ||
            app.IsCodexVoiceCaptureActive() ||
            app.IsCodexRealtimePlaybackActive();
        if (!system_claimed && codec != nullptr) codec->EnableOutput(false);
        const bool realtime_playback = app.IsCodexRealtimePlaybackActive();
        app.GetAudioService().SetExternalPlaybackActive(realtime_playback);
        if (!system_claimed) app.RestoreSystemAudioAfterStressTest();
        focus_acquired = false;
    }

    void Complete(bool error) {
        RestoreRoute();
        renderer.Reset();
        codec = nullptr;
        release_samples = 0;
        release_in_progress = false;
        portENTER_CRITICAL(&state_mux);
        start_requested = false;
        release_requested = false;
        interrupted = false;
        public_state.store(error ? METALIO_APP_SYNTH_ERROR
                                 : METALIO_APP_SYNTH_IDLE,
                           std::memory_order_release);
        active.store(false, std::memory_order_release);
        releasing.store(false, std::memory_order_release);
        portEXIT_CRITICAL(&state_mux);
        CompleteAssistantCallback(!error);
    }

    bool StartWorker() {
        // The HLS pipeline and its stop task must be gone before this worker
        // takes the shared I2S route. The potentially blocking wait stays off
        // the UI thread.
        if (MediaService* media = MediaService::Existing()) {
            if (!media->ResetForAppLaunch(6000)) return false;
        }
        if (!RouteStillWanted() || !CanAcquire()) return false;

        codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr || !RouteStillWanted()) return false;

        Application& app = Application::GetInstance();
        app.StopSystemAudioForStressTest();
        focus_acquired = true;
        if (!RouteStillWanted() || !CanAcquire()) {
            RestoreRoute();
            return false;
        }
        app.GetAudioService().SetExternalPlaybackActive(true);
        codec->EnableOutput(true);
        if (!RouteStillWanted()) {
            RestoreRoute();
            return false;
        }

        renderer.Reset();
        release_in_progress = false;
        Snapshot snapshot = CopySnapshot();
        renderer.SetTarget(snapshot.params_valid ? snapshot.params
                                                 : DefaultParams());
        public_state.store(METALIO_APP_SYNTH_RUNNING,
                           std::memory_order_release);
        ESP_LOGI(kTag, "synth audio route acquired");
        return true;
    }

    void BeginRelease(uint16_t ramp_ms, bool interrupted_request) {
        if (release_in_progress) return;
        if (!focus_acquired) {
            Complete(false);
            return;
        }
        release_in_progress = true;
        if (interrupted_request) ramp_ms = std::min<uint16_t>(ramp_ms, 40U);
        ramp_ms = std::max<uint16_t>(ramp_ms, 5U);
        renderer.RampToSilence(ramp_ms);
        release_samples = static_cast<uint32_t>(ramp_ms) * kSampleRate / 1000U;
    }

    static void WorkerEntry(void* argument) {
        static_cast<Impl*>(argument)->WorkerMain();
    }

    void WorkerMain() {
        TickType_t last_wake = xTaskGetTickCount();
        while (true) {
            if (!active.load(std::memory_order_acquire)) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                last_wake = xTaskGetTickCount();
                continue;
            }
            Snapshot snapshot = CopySnapshot();

            if (snapshot.start_requested && active.load(std::memory_order_acquire)) {
                if (snapshot.release_requested) {
                    Complete(false);
                    continue;
                }
                if (!StartWorker()) {
                    Complete(RouteStillWanted() && CanAcquire());
                    continue;
                }
                last_wake = xTaskGetTickCount();
                snapshot = CopySnapshot();
            }

            if (!active.load(std::memory_order_acquire)) continue;
            if (focus_acquired && !release_in_progress &&
                !snapshot.release_requested && !CanAcquire()) {
                RequestRelease(nullptr, false, true);
                snapshot = CopySnapshot();
            }
            if (snapshot.params_dirty && focus_acquired &&
                !release_in_progress && !snapshot.release_requested) {
                renderer.SetTarget(snapshot.params);
            }
            if (snapshot.release_requested) {
                BeginRelease(snapshot.params.level_ramp_ms,
                             snapshot.interrupted);
                // Mark this release as consumed. A concurrent repeated stop
                // remains harmless; the worker keeps ticking to silence.
                portENTER_CRITICAL(&state_mux);
                release_requested = false;
                portEXIT_CRITICAL(&state_mux);
            }

            if (!focus_acquired) {
                Complete(false);
                continue;
            }

            renderer.Render(pcm.data(), pcm.size());
            codec->OutputData(pcm);
            if (release_samples > 0) {
                release_samples = release_samples > kFramesPerBlock
                                      ? release_samples - kFramesPerBlock
                                      : 0;
                if (release_samples == 0) {
                    Complete(false);
                    continue;
                }
            }
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kBlockMs));
        }
    }

    bool RequestRelease(void* requested_owner, bool clear_owner,
                        bool is_interruption) {
        portENTER_CRITICAL(&state_mux);
        const bool same_owner = requested_owner == nullptr ||
                                requested_owner == owner;
        if (active.load(std::memory_order_relaxed) && !same_owner) {
            portEXIT_CRITICAL(&state_mux);
            return false;
        }
        if (!active.load(std::memory_order_relaxed)) {
            if (clear_owner && same_owner) {
                owner = nullptr;
                params_valid = false;
            }
            portEXIT_CRITICAL(&state_mux);
            return true;
        }
        release_requested = true;
        releasing.store(true, std::memory_order_release);
        interrupted = interrupted || is_interruption;
        if (clear_owner) owner = nullptr;
        portEXIT_CRITICAL(&state_mux);
        Notify();
        return true;
    }
};

SynthService& SynthService::Get() {
    static SynthService service;
    return service;
}

SynthService* SynthService::Existing() {
    return s_existing_service.load(std::memory_order_acquire);
}

SynthService::SynthService() : impl_(new (std::nothrow) Impl()) {
    s_existing_service.store(this, std::memory_order_release);
}

bool SynthService::Available() const {
    return impl_ != nullptr &&
           impl_->worker_ready.load(std::memory_order_acquire);
}

bool SynthService::IsActive() const {
    return impl_ != nullptr && impl_->active.load(std::memory_order_acquire);
}

int SynthService::Start(void* owner) {
    if (owner == nullptr) return METALIO_APP_SYNTH_ERROR_INVALID;
    if (!Available()) return METALIO_APP_SYNTH_ERROR_AUDIO;
    if (impl_->active.load(std::memory_order_acquire)) {
        portENTER_CRITICAL(&impl_->state_mux);
        const bool same_owner = impl_->owner == owner;
        const bool releasing = impl_->releasing.load(std::memory_order_relaxed);
        portEXIT_CRITICAL(&impl_->state_mux);
        return same_owner && !releasing ? METALIO_APP_SYNTH_OK
                                        : METALIO_APP_SYNTH_ERROR_BUSY;
    }
    Application& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle ||
        app.IsLowPowerStandby() ||
        app.IsCodexVoiceCaptureActive() ||
        app.IsCodexRealtimePlaybackActive() ||
        !ai::Availability::Get().IsAvailable() ||
        (RecordingService::Existing() != nullptr &&
         RecordingService::Existing()->IsActive())) {
        return METALIO_APP_SYNTH_ERROR_BUSY;
    }
    portENTER_CRITICAL(&impl_->state_mux);
    if (impl_->active.load(std::memory_order_relaxed)) {
        const bool same_owner = impl_->owner == owner;
        const bool releasing = impl_->releasing.load(std::memory_order_relaxed);
        portEXIT_CRITICAL(&impl_->state_mux);
        return same_owner && !releasing ? METALIO_APP_SYNTH_OK
                                        : METALIO_APP_SYNTH_ERROR_BUSY;
    }
    if (impl_->owner != owner) {
        impl_->owner = owner;
        impl_->params = DefaultParams();
        impl_->params_valid = false;
        impl_->params_dirty = false;
    }
    impl_->release_requested = false;
    impl_->releasing.store(false, std::memory_order_release);
    impl_->interrupted = false;
    impl_->start_requested = true;
    impl_->params_dirty = false;
    impl_->public_state.store(METALIO_APP_SYNTH_STARTING,
                              std::memory_order_release);
    impl_->active.store(true, std::memory_order_release);
    portEXIT_CRITICAL(&impl_->state_mux);
    impl_->Notify();
    return METALIO_APP_SYNTH_OK;
}

int SynthService::Set(void* owner,
                      const metalio_app_synth_params_t* params) {
    if (owner == nullptr || !ValidParams(params)) {
        return METALIO_APP_SYNTH_ERROR_INVALID;
    }
    if (impl_ == nullptr) return METALIO_APP_SYNTH_ERROR_AUDIO;
    portENTER_CRITICAL(&impl_->state_mux);
    if (impl_->active.load(std::memory_order_relaxed) &&
        impl_->owner != owner) {
        portEXIT_CRITICAL(&impl_->state_mux);
        return METALIO_APP_SYNTH_ERROR_BUSY;
    }
    if (impl_->owner != owner) {
        impl_->owner = owner;
        impl_->params = DefaultParams();
        impl_->params_valid = false;
        impl_->params_dirty = false;
    }
    impl_->params = *params;
    impl_->params_valid = true;
    impl_->params_dirty = true;
    const bool active = impl_->active.load(std::memory_order_relaxed);
    portEXIT_CRITICAL(&impl_->state_mux);
    if (active) impl_->Notify();
    return METALIO_APP_SYNTH_OK;
}

int SynthService::Stop(void* owner) {
    if (owner == nullptr) return METALIO_APP_SYNTH_ERROR_INVALID;
    if (impl_ == nullptr) return METALIO_APP_SYNTH_ERROR_AUDIO;
    return impl_->RequestRelease(owner, false, false)
               ? METALIO_APP_SYNTH_OK
               : METALIO_APP_SYNTH_ERROR_INVALID;
}

int SynthService::GetState(void* owner,
                           metalio_app_synth_state_t* state) const {
    if (owner == nullptr || state == nullptr) {
        return METALIO_APP_SYNTH_ERROR_INVALID;
    }
    if (impl_ == nullptr) return METALIO_APP_SYNTH_ERROR_AUDIO;
    portENTER_CRITICAL(&impl_->state_mux);
    const bool active = impl_->active.load(std::memory_order_relaxed);
    const bool matches = impl_->owner == owner;
    const metalio_app_synth_state_t current =
        !active && !matches ? METALIO_APP_SYNTH_IDLE
                            : impl_->public_state.load(std::memory_order_relaxed);
    portEXIT_CRITICAL(&impl_->state_mux);
    if (active && !matches) return METALIO_APP_SYNTH_ERROR_INVALID;
    *state = current;
    return METALIO_APP_SYNTH_OK;
}

void SynthService::SuspendOwner(void* owner) {
    if (owner != nullptr && impl_ != nullptr) {
        impl_->RequestRelease(owner, false, true);
    }
}

void SynthService::UnloadOwner(void* owner) {
    if (owner != nullptr && impl_ != nullptr) {
        impl_->RequestRelease(owner, true, true);
    }
}

void SynthService::Interrupt() {
    if (impl_ != nullptr) impl_->RequestRelease(nullptr, false, true);
}

bool SynthService::ResetForAppLaunch(uint32_t timeout_ms) {
    if (impl_ == nullptr) return true;
    impl_->RequestRelease(nullptr, true, true);
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (impl_->active.load(std::memory_order_acquire)) {
        if (static_cast<TickType_t>(xTaskGetTickCount() - started) >=
            timeout_ticks) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool SynthService::BeginAssistantInteraction(
        std::function<void(bool)> ready) {
    if (impl_ == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->callback_mutex);
        portENTER_CRITICAL(&impl_->state_mux);
        const bool active = impl_->active.load(std::memory_order_relaxed);
        if (!active) {
            portEXIT_CRITICAL(&impl_->state_mux);
            return false;
        }
        impl_->assistant_callback = std::move(ready);
        portEXIT_CRITICAL(&impl_->state_mux);
    }
    impl_->RequestRelease(nullptr, false, true);
    return true;
}

}  // namespace agent_ui::external_apps
