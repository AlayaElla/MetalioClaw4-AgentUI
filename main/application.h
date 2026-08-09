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
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void StartCodexVoiceCapture();
    void StopCodexVoiceCapture(std::function<void()> on_stopped = {});
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
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
    std::atomic<bool> codex_voice_capture_active_{false};
    bool codex_voice_stop_pending_ = false;
    bool codex_voice_restore_wake_word_ = false;
    std::function<void()> codex_voice_stopped_callback_;
    std::atomic<bool> low_power_standby_{false};
    bool standby_restore_wake_word_ = false;

    bool has_server_time_ = false;
    bool aborted_ = false;
    int clock_ticks_ = 0;
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
