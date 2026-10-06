#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <atomic>
#include <functional>
#include <mutex>
#include <deque>
#include <memory>

#include "protocol.h"
#include "provisioning_client.h"
#include "audio_service.h"
#include "device_state_event.h"
#include "codex_battery_reporter.h"
#include "ai/ai_availability.h"


#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

enum class SpecialInteraction {
    None,
    Charging,
    Sleep,
    Dizzy,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void ScheduleAi(std::function<void()> callback);
    void SetAiWakeEnabled(bool enabled);
    bool IsAiWakeEnabled() const { return ai_wake_enabled_.load(); }
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void StartCodexVoiceCapture();
    void StopCodexVoiceCapture(std::function<void()> on_stopped = {});
    // Realtime capture uses the same 16 kHz / 60 ms encoder, but every
    // packet is envelope-bound to this request instead of the legacy binary
    // WebSocket path.
    void StartCodexRealtimeCapture(const std::string& request_id);
    // Pause only microphone uplink; playback and the request sequence survive
    // a half-duplex speaking/thinking transition.
    void StopCodexRealtimeCapture();
    void EndCodexRealtimeSession();
    bool PushCodexRealtimeAudio(std::unique_ptr<AudioStreamPacket> packet);
    void ClearCodexRealtimeAudio();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    // Returns false while voice capture or another audio stream owns output,
    // allowing a notification caller to defer without disrupting speech.
    bool PlayCodexNotificationSound(const std::string_view& sound,
                                    uint8_t gain_percent);
    AudioService& GetAudioService() { return audio_service_; }
    bool HasPendingActivation() const {
        return !activation_suspended_ && !pending_activation_code_.empty();
    }
    const std::string& GetPendingActivationCode() const { return pending_activation_code_; }
    void SetActivationSuspended(bool suspended);
    bool IsActivationSuspended() const { return activation_suspended_; }
    void StopSystemAudioForStressTest();
    void RestoreSystemAudioAfterStressTest();
    void TriggerSpecialInteraction(SpecialInteraction interaction, int detail = -1);

    void ForceReturnToIdle();
    void SetLowPowerStandby(bool enabled);
    bool IsLowPowerStandby() const { return low_power_standby_.load(); }
    bool IsCodexVoiceCaptureActive() const {
        return codex_voice_capture_active_.load();
    }
    bool IsCodexRealtimePlaybackActive() const {
        return codex_realtime_playback_active_.load();
    }
private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::string pending_activation_code_;
    volatile bool activation_suspended_ = false;
    bool codex_voice_start_pending_ = false;
    int64_t codex_voice_start_wait_started_at_us_ = 0;
    std::atomic<bool> codex_voice_capture_active_{false};
    bool codex_voice_stop_pending_ = false;
    int64_t codex_voice_stop_wait_started_at_us_ = 0;
    bool codex_voice_restore_wake_word_ = false;
    std::function<void()> codex_voice_stopped_callback_;
    std::string codex_realtime_request_id_;
    uint32_t codex_realtime_audio_sequence_ = 0;
    std::atomic<bool> codex_realtime_playback_active_{false};
    bool codex_realtime_restore_wake_word_ = false;
    std::atomic<bool> low_power_standby_{false};
    bool standby_restore_wake_word_ = false;
    std::atomic<bool> ai_wake_enabled_{true};
    ai::Availability::Token standby_ai_block_ = 0;
    ai::Availability::Token codex_voice_ai_block_ = 0;
    bool audio_initialized_ = false;
    std::atomic<bool> assistant_listen_pending_{false};
    void ApplyAiAvailability();
    bool DeferAssistantForMedia(std::function<void()> continuation);

    bool has_server_time_ = false;
    bool aborted_ = false;
    int clock_ticks_ = 0;
    codex_battery::Reporter codex_battery_reporter_;
    TaskHandle_t main_event_loop_task_handle_ = nullptr;
    SpecialInteraction active_special_interaction_ = SpecialInteraction::None;

    void OnWakeWordDetected();
    void ProvisionDevice(ProvisioningClient& client);
    void CheckAssetsVersion();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    void FinishSpecialInteraction(bool restore_sleep);
    void CancelSpecialInteraction();
    void TryStartCodexVoiceCapture();
    void TryFinishCodexVoiceCapture();
    void FailCodexVoiceCaptureTransport();
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
