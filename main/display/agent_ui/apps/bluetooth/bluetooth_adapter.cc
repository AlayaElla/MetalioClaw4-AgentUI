#include "bluetooth_adapter.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "IOExpander.hpp"
#include "SimpleUart.hpp"
#include "application.h"
#include "audio_output_route.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "settings.h"

namespace agent_ui::bluetooth {
namespace {

constexpr const char* TAG = "BluetoothAdapter";
constexpr const char* kSettingsNamespace = "bluetooth";
constexpr const char* kRememberedAddressKey = "device_addr";
constexpr const char* kRememberedNameKey = "device_name";
constexpr int kAddressLength = 12;
constexpr int kBluetoothVolumeMax = 12;
constexpr EventBits_t kMode1Ready = BIT0;
constexpr EventBits_t kMode2Ready = BIT1;
constexpr EventBits_t kMode3Ready = BIT2;
constexpr EventBits_t kInquiryStarted = BIT3;

enum class ModuleMode : uint8_t {
    None,
    Local,
    Transmitter,
    Receiver,
};

bool IsHex(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

bool IsValidAddress(const std::string& address) {
    if (address.size() != kAddressLength) return false;
    return std::all_of(address.begin(), address.end(), IsHex);
}

int ToBluetoothVolume(int device_volume) {
    const int normalized = std::clamp(device_volume, 0, 100);
    return (normalized * kBluetoothVolumeMax + 50) / 100;
}

void Trim(std::string& value) {
    while (!value.empty() &&
           (value.back() == '\r' || value.back() == '\n' ||
            value.back() == ' ')) {
        value.pop_back();
    }
    const std::size_t first = value.find_first_not_of(' ');
    value = first == std::string::npos ? std::string() : value.substr(first);
}

EventBits_t ReadyBit(ModuleMode mode) {
    switch (mode) {
        case ModuleMode::Local:
            return kMode1Ready;
        case ModuleMode::Transmitter:
            return kMode2Ready;
        case ModuleMode::Receiver:
            return kMode3Ready;
        case ModuleMode::None:
            return 0;
    }
    return 0;
}

}  // namespace

struct Adapter::Impl {
    std::mutex mutex;
    EventSink event_sink;
    Event snapshot;
    std::string rx_buffer;
    Device pending_device;
    Device reconnect_device;
    EventGroupHandle_t events = nullptr;
    std::atomic<bool> initialized{false};
    std::atomic<bool> mode_command_running{false};
    std::atomic<bool> reset_running{false};
    std::atomic<bool> reset_reenable_pending{false};
    std::atomic<bool> profile_command_running{false};
    std::atomic<bool> volume_sync_running{false};
    std::atomic<bool> sco_validation_running{false};
    std::atomic<bool> sco_input_ready{false};
    std::atomic<bool> local_recovery_running{false};
    std::atomic<bool> wake_word_paused{false};
    std::atomic<bool> manual_connect_expected{false};
    std::atomic<bool> module_reconnect_expected{false};
    std::atomic<bool> scan_after_mode_ready{false};
    std::atomic<bool> scan_expected{false};
    std::atomic<bool> slc_connected{false};
    std::atomic<bool> sco_connected{false};
    std::atomic<uint32_t> audio_session{0};
    std::atomic<int> module_volume{-1};
    std::atomic<int> desired_module_volume{ToBluetoothVolume(70)};
    std::atomic<AudioProfile> requested_profile{AudioProfile::None};
    std::atomic<ModuleMode> requested_mode{ModuleMode::None};
    std::atomic<ModuleMode> active_mode{ModuleMode::None};

    void Publish() {
        Event event;
        EventSink sink;
        {
            std::lock_guard<std::mutex> lock(mutex);
            event = snapshot;
            sink = event_sink;
        }
        if (sink) sink(event);
    }

    void SetSink(EventSink sink) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            event_sink = std::move(sink);
        }
        Publish();
    }

    void LoadSettings() {
        Settings audio_settings("audio", false);
        desired_module_volume.store(ToBluetoothVolume(
            audio_settings.GetInt("output_volume", 70)));
    }

    void PauseWakeWord() {
        auto& service = Application::GetInstance().GetAudioService();
        if (!service.IsWakeWordRunning()) return;
        bool expected = false;
        if (wake_word_paused.compare_exchange_strong(expected, true)) {
            ESP_LOGI(TAG, "Pausing wake word while Bluetooth owns the I2S input");
            service.EnableWakeWordDetection(false);
        }
    }

    void ResumeWakeWord() {
        if (!wake_word_paused.exchange(false)) return;
        ESP_LOGI(TAG, "Resuming wake word after the I2S input is ready");
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(
            true);
    }

    void ClearConnectionExpectations() {
        manual_connect_expected.store(false);
        module_reconnect_expected.store(false);
    }

    void ClearProfileState() {
        audio_session.fetch_add(1);
        requested_profile.store(AudioProfile::None);
        slc_connected.store(false);
        sco_connected.store(false);
        sco_validation_running.store(false);
        sco_input_ready.store(false);
        module_volume.store(-1);
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.audio_profile = AudioProfile::None;
    }

    void UseLocalRoute(bool disable_setting) {
        auto& application = Application::GetInstance();
        application.GetAudioService().ResetDecoder();
        auto* codec = Board::GetInstance().GetAudioCodec();
        const bool restart_output = codec != nullptr && codec->output_enabled();
        if (restart_output) {
            codec->EnableOutput(false);
        }
        AudioOutput_SetTarget(AudioOutputTarget::LocalSpeaker,
                              disable_setting);
        if (restart_output) {
            codec->EnableOutput(true);
        }
        ClearConnectionExpectations();
        ClearProfileState();
        scan_expected.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (disable_setting) snapshot.enabled = false;
            if (disable_setting && !reset_running.load()) {
                snapshot.resetting = false;
            }
            snapshot.scanning = false;
            snapshot.connection = ConnectionState::Idle;
            snapshot.has_current_device = false;
            snapshot.current_device = {};
            if (disable_setting) snapshot.nearby_devices.clear();
        }
        Publish();
    }

    bool ParseDevice(const std::string& line, Device& device) const {
        constexpr const char* prefix = "AT+BT:";
        if (line.rfind(prefix, 0) != 0) return false;
        const std::string payload = line.substr(std::strlen(prefix));
        if (payload.size() < kAddressLength) return false;
        const std::string address = payload.substr(0, kAddressLength);
        if (!IsValidAddress(address)) return false;
        device.address = address;
        device.name = payload.substr(kAddressLength);
        return true;
    }

    bool MarkModeReady(ModuleMode mode) {
        if (events != nullptr) xEventGroupSetBits(events, ReadyBit(mode));
        if (requested_mode.load() != mode) {
            ESP_LOGI(TAG, "Ignoring stale Bluetooth mode %d acknowledgement",
                     static_cast<int>(mode));
            return false;
        }
        active_mode.store(mode);
        if (mode == ModuleMode::Local) local_recovery_running.store(false);
        const bool reset_completed =
            mode == ModuleMode::Transmitter &&
            reset_reenable_pending.exchange(false);
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.enabled = mode == ModuleMode::Transmitter;
            if (reset_completed) snapshot.resetting = false;
            if (mode != ModuleMode::Transmitter) {
                snapshot.scanning = false;
                snapshot.connection = ConnectionState::Idle;
            }
        }
        Publish();
        return true;
    }

    void HandleReconnect(const std::string& line) {
        if (requested_mode.load() != ModuleMode::Transmitter) {
            ESP_LOGW(TAG, "Ignoring reconnect outside Bluetooth speaker mode: %s",
                     line.c_str());
            return;
        }
        constexpr const char* prefix = "RECONNECT DEVICE NAME:";
        std::string name;
        const std::size_t name_offset = line.find(prefix);
        if (name_offset != std::string::npos) {
            name = line.substr(name_offset + std::strlen(prefix));
            Trim(name);
        }
        if (name.empty()) name = "Bluetooth device";

        scan_after_mode_ready.store(false);
        scan_expected.store(false);
        module_reconnect_expected.store(true);
        MarkModeReady(ModuleMode::Transmitter);
        {
            std::lock_guard<std::mutex> lock(mutex);
            reconnect_device = {.name = name};
            snapshot.enabled = true;
            snapshot.scanning = false;
            snapshot.connection = ConnectionState::Connecting;
            snapshot.nearby_devices.clear();
        }
        Publish();
        ESP_LOGI(TAG, "Bluetooth module is reconnecting: %s",
                 name.c_str());
    }

    void HandleConnectSuccess() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (snapshot.connection == ConnectionState::Connected &&
                snapshot.has_current_device) {
                return;
            }
        }
        Device connected;
        if (manual_connect_expected.exchange(false)) {
            std::lock_guard<std::mutex> lock(mutex);
            connected = pending_device;
        } else if (module_reconnect_expected.exchange(false)) {
            std::lock_guard<std::mutex> lock(mutex);
            connected = reconnect_device;
        } else {
            ESP_LOGW(TAG,
                     "Ignoring uncorrelated Bluetooth connection success");
            return;
        }
        ClearConnectionExpectations();
        scan_expected.store(false);
        requested_profile.store(AudioProfile::None);
        slc_connected.store(false);
        sco_connected.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.enabled = true;
            snapshot.scanning = false;
            snapshot.connection = ConnectionState::Connected;
            snapshot.has_current_device = true;
            snapshot.current_device = connected;
            snapshot.nearby_devices.clear();
            snapshot.audio_profile = AudioProfile::None;
        }
        Publish();
    }

    struct VolumeSyncArgs {
        Impl* self;
        uint32_t session;
    };

    static void VolumeSyncTask(void* parameter) {
        auto* args = static_cast<VolumeSyncArgs*>(parameter);
        Impl* self = args->self;
        const uint32_t session = args->session;
        delete args;

        while (self->audio_session.load() == session) {
            bool connected = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                connected = self->snapshot.connection ==
                            ConnectionState::Connected;
            }
            const int current = self->module_volume.load();
            const int desired = self->desired_module_volume.load();
            if (!connected || current < 0 || current == desired) break;

            const bool increase = current < desired;
            const char* command = increase ? "AT+VOLUP\r\n"
                                           : "AT+VOLDOWN\r\n";
            if (!SimpleUart::getInstance().sendString(command)) break;
            ESP_LOGI(TAG, "TX: %s (module volume %d -> %d)",
                     increase ? "AT+VOLUP" : "AT+VOLDOWN", current,
                     desired);
            self->module_volume.store(current + (increase ? 1 : -1));
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        self->volume_sync_running.store(false);
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            connected = self->snapshot.connection == ConnectionState::Connected;
        }
        if (connected && self->module_volume.load() >= 0 &&
            self->module_volume.load() != self->desired_module_volume.load()) {
            self->ScheduleVolumeSync();
        }
        vTaskDelete(nullptr);
    }

    void ScheduleVolumeSync() {
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = snapshot.connection == ConnectionState::Connected;
        }
        if (!connected || module_volume.load() < 0 ||
            module_volume.load() == desired_module_volume.load()) {
            return;
        }
        bool expected = false;
        if (!volume_sync_running.compare_exchange_strong(expected, true)) {
            return;
        }
        auto* args = new VolumeSyncArgs{this, audio_session.load()};
        if (xTaskCreate(VolumeSyncTask, "bt_volume", 3072, args, 5, nullptr) !=
            pdPASS) {
            delete args;
            volume_sync_running.store(false);
            ESP_LOGE(TAG, "Failed to create Bluetooth volume sync task");
        }
    }

    void SetDeviceVolume(int volume) {
        desired_module_volume.store(ToBluetoothVolume(volume));
        ScheduleVolumeSync();
    }

    void HandleSlcConnectSuccess() {
        HandleConnectSuccess();
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = snapshot.connection == ConnectionState::Connected;
        }
        if (!connected) return;
        slc_connected.store(true);
        ESP_LOGI(TAG, "Bluetooth hands-free control link is ready");
        SetAudioProfile(AudioProfile::Call);
    }

    struct ScoValidationArgs {
        Impl* self;
        uint32_t session;
        bool require_sco;
    };

    static void ScoValidationTask(void* parameter) {
        auto* args = static_cast<ScoValidationArgs*>(parameter);
        Impl* self = args->self;
        const uint32_t session = args->session;
        const bool require_sco = args->require_sco;
        delete args;

        vTaskDelay(pdMS_TO_TICKS(150));
        auto& service = Application::GetInstance().GetAudioService();
        constexpr int kInputProbeSamples = 512;
        constexpr int kRequiredConsecutiveFrames = 5;
        constexpr int kInputProbeAttempts = 10;
        int consecutive_frames = 0;
        int microphone_peak = 0;
        int reference_peak = 0;
        for (int attempt = 1; attempt <= kInputProbeAttempts; ++attempt) {
            bool connected = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                connected = self->snapshot.connection ==
                            ConnectionState::Connected;
            }
            if (self->audio_session.load() != session || !connected ||
                (require_sco && !self->sco_connected.load())) {
                vTaskDelete(nullptr);
                return;
            }
            std::vector<int16_t> probe;
            if (service.ReadAudioData(probe, 16000, kInputProbeSamples)) {
                ++consecutive_frames;
                for (std::size_t i = 0; i + 1 < probe.size(); i += 2) {
                    const int microphone = probe[i];
                    const int reference = probe[i + 1];
                    microphone_peak = std::max(
                        microphone_peak,
                        microphone < 0 ? -microphone : microphone);
                    reference_peak = std::max(
                        reference_peak,
                        reference < 0 ? -reference : reference);
                }
            } else {
                consecutive_frames = 0;
            }
            if (consecutive_frames >= kRequiredConsecutiveFrames) {
                self->sco_input_ready.store(true);
                self->sco_validation_running.store(false);
                self->ResumeWakeWord();
                self->StartInputWatchdog(session);
                ESP_LOGI(TAG,
                         "Bluetooth I2S input is stable: microphone_peak=%d "
                         "reference_peak=%d",
                         microphone_peak, reference_peak);
                vTaskDelete(nullptr);
                return;
            }
        }

        if (self->audio_session.load() == session) {
            self->sco_validation_running.store(false);
            ESP_LOGE(TAG,
                     "Bluetooth SCO has no readable I2S input; wake word remains paused");
        }
        vTaskDelete(nullptr);
    }

    void ValidateAudioInput(bool require_sco) {
        if (sco_input_ready.load()) return;
        bool expected = false;
        if (!sco_validation_running.compare_exchange_strong(expected, true)) {
            return;
        }
        auto* args =
            new ScoValidationArgs{this, audio_session.load(), require_sco};
        if (xTaskCreate(ScoValidationTask, "bt_sco_input", 4096, args, 5,
                        nullptr) != pdPASS) {
            delete args;
            sco_validation_running.store(false);
            ESP_LOGE(TAG, "Failed to create Bluetooth SCO validation task");
        }
    }

    void HandleScoReady() {
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = snapshot.connection == ConnectionState::Connected;
        }
        if (!connected) return;
        sco_connected.store(true);
        sco_input_ready.store(false);
        requested_profile.store(AudioProfile::Call);
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.audio_profile = AudioProfile::Call;
        }
        AudioOutput_SetTarget(AudioOutputTarget::BluetoothSpeaker, false);
        if (module_volume.load() < 0) {
            module_volume.store(0);
        }
        ScheduleVolumeSync();
        Publish();
        ESP_LOGI(TAG, "Bluetooth SCO link is ready; validating I2S input");
        ValidateAudioInput(true);
    }

    void HandleDeviceDisconnected(const std::string& reason) {
        if (reset_running.load()) return;
        bool expected = false;
        if (!local_recovery_running.compare_exchange_strong(expected, true)) {
            return;
        }
        ESP_LOGI(TAG, "Bluetooth device disconnected: %s", reason.c_str());
        UseLocalRoute(true);
        StartMode(ModuleMode::Local);
    }

    struct InputWatchdogArgs {
        Impl* self;
        uint32_t session;
    };

    static void InputWatchdogTask(void* parameter) {
        auto* args = static_cast<InputWatchdogArgs*>(parameter);
        Impl* self = args->self;
        const uint32_t session = args->session;
        delete args;

        auto& service = Application::GetInstance().GetAudioService();
        uint32_t input_sequence = service.GetInputSequence();
        constexpr int kInputWatchdogIntervalMs = 250;
        constexpr int kInputWatchdogMissLimit = 8;
        int missed_intervals = 0;

        while (self->audio_session.load() == session) {
            vTaskDelay(pdMS_TO_TICKS(kInputWatchdogIntervalMs));
            bool connected = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                connected = self->snapshot.connection ==
                            ConnectionState::Connected;
            }
            if (!connected ||
                self->requested_profile.load() != AudioProfile::Call ||
                !self->sco_connected.load()) {
                break;
            }
            if (!service.IsWakeWordRunning() &&
                !service.IsAudioProcessorRunning()) {
                input_sequence = service.GetInputSequence();
                missed_intervals = 0;
                continue;
            }

            const uint32_t current_sequence = service.GetInputSequence();
            if (current_sequence != input_sequence) {
                input_sequence = current_sequence;
                missed_intervals = 0;
                continue;
            }
            if (++missed_intervals >= kInputWatchdogMissLimit) {
                self->HandleDeviceDisconnected("Bluetooth I2S input stopped");
                break;
            }
        }
        vTaskDelete(nullptr);
    }

    void StartInputWatchdog(uint32_t session) {
        auto* args = new InputWatchdogArgs{this, session};
        if (xTaskCreate(InputWatchdogTask, "bt_input_watch", 4096, args, 5,
                        nullptr) != pdPASS) {
            delete args;
            ESP_LOGE(TAG, "Failed to create Bluetooth input watchdog task");
        }
    }

    void HandleLine(const std::string& raw_line) {
        std::string line = raw_line;
        Trim(line);
        if (line.empty()) return;
        ESP_LOGI(TAG, "RX: %s", line.c_str());

        if (line.find("SET MODE 1") != std::string::npos) {
            if (MarkModeReady(ModuleMode::Local)) ResumeWakeWord();
            return;
        }
        if (line.find("SET MODE 2") != std::string::npos) {
            MarkModeReady(ModuleMode::Transmitter);
            return;
        }
        if (line.find("SET MODE 3") != std::string::npos) {
            MarkModeReady(ModuleMode::Receiver);
            return;
        }
        if (line.find("RECONNECT") != std::string::npos) {
            HandleReconnect(line);
            return;
        }
        if (line.find("DISCONNECT") != std::string::npos ||
            line.find("SLC DISC") != std::string::npos ||
            line.find("LINK LOSS") != std::string::npos ||
            line.find("LOST LINK") != std::string::npos ||
            line.find("CONNECTION LOST") != std::string::npos) {
            HandleDeviceDisconnected(line);
            return;
        }
        if (line.find("INQUIRING START") != std::string::npos) {
            if (!scan_expected.load()) return;
            if (events != nullptr) xEventGroupSetBits(events, kInquiryStarted);
            {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.scanning = true;
                snapshot.connection = ConnectionState::Scanning;
                snapshot.nearby_devices.clear();
            }
            Publish();
            return;
        }
        Device device;
        if (ParseDevice(line, device)) {
            if (!scan_expected.load()) return;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto duplicate = std::find(
                    snapshot.nearby_devices.begin(),
                    snapshot.nearby_devices.end(), device);
                if (duplicate == snapshot.nearby_devices.end()) {
                    snapshot.nearby_devices.push_back(std::move(device));
                }
            }
            Publish();
            return;
        }
        if (line.find("INQ COMPLETE") != std::string::npos) {
            if (!scan_expected.exchange(false)) return;
            {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.scanning = false;
                if (snapshot.connection == ConnectionState::Scanning) {
                    snapshot.connection = ConnectionState::Idle;
                }
            }
            Publish();
            return;
        }
        if (line.find("CONNECTING") != std::string::npos) {
            if (!manual_connect_expected.load()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.connection = ConnectionState::Connecting;
            }
            Publish();
            return;
        }
        if (line.find("SLC CONNECT SUCCESS") != std::string::npos) {
            HandleSlcConnectSuccess();
            return;
        }
        if (line.find("SETUP SCO") != std::string::npos) {
            HandleScoReady();
            return;
        }
        if (line.find("DISC SCO") != std::string::npos) {
            if (requested_profile.load() != AudioProfile::Music) {
                HandleDeviceDisconnected(line);
                return;
            }
            sco_connected.store(false);
            requested_profile.store(AudioProfile::Music);
            {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.audio_profile = AudioProfile::Music;
            }
            sco_input_ready.store(false);
            Publish();
            PauseWakeWord();
            ValidateAudioInput(false);
            return;
        }
        if (line.find("CONNECT SUCCESS") != std::string::npos) {
            HandleConnectSuccess();
            return;
        }
        if (line.find("CONNECT TIMEOUT") != std::string::npos) {
            HandleDeviceDisconnected(line);
            return;
        }

        int volume = -1;
        if (std::sscanf(line.c_str(), "AT+VOL=%d", &volume) == 1) {
            module_volume.store(std::clamp(volume, 0, kBluetoothVolumeMax));
            ScheduleVolumeSync();
        }
    }

    void OnUartData(const std::vector<uint8_t>& data) {
        rx_buffer.append(data.begin(), data.end());
        std::size_t consumed = 0;
        while (true) {
            const std::size_t delimiter = rx_buffer.find_first_of(
                "\r\n", consumed);
            if (delimiter == std::string::npos) break;
            HandleLine(rx_buffer.substr(consumed, delimiter - consumed));
            consumed = delimiter + 1;
            while (consumed < rx_buffer.size() &&
                   (rx_buffer[consumed] == '\r' ||
                    rx_buffer[consumed] == '\n')) {
                ++consumed;
            }
        }
        if (consumed > 0) rx_buffer.erase(0, consumed);
        if (rx_buffer.size() > 2048) rx_buffer.clear();
    }

    struct ModeTaskArgs {
        Impl* self;
        ModuleMode mode;
    };

    static void ModeTask(void* parameter) {
        auto* args = static_cast<ModeTaskArgs*>(parameter);
        Impl* self = args->self;
        const ModuleMode mode = args->mode;
        delete args;

        const char* prepare = nullptr;
        const char* command = nullptr;
        switch (mode) {
            case ModuleMode::Local:
                prepare = "AT+RX=2\r\n";
                command = "AT+MODE=1\r\n";
                break;
            case ModuleMode::Transmitter:
                prepare = "AT+TX=1\r\n";
                command = "AT+MODE=2\r\n";
                break;
            case ModuleMode::Receiver:
                prepare = "AT+RX=1\r\n";
                command = "AT+MODE=3\r\n";
                break;
            case ModuleMode::None:
                self->mode_command_running.store(false);
                vTaskDelete(nullptr);
                return;
        }

        SimpleUart& uart = SimpleUart::getInstance();
        const EventBits_t ready = ReadyBit(mode);
        constexpr int kModeCommandAttempts = 3;
        bool acknowledged = false;
        for (int attempt = 1; attempt <= kModeCommandAttempts; ++attempt) {
            xEventGroupClearBits(self->events, ready);
            const bool prepared = uart.sendString(prepare);
            ESP_LOGI(TAG, "TX: %.*s (attempt %d/%d)",
                     static_cast<int>(std::strlen(prepare) - 2), prepare,
                     attempt, kModeCommandAttempts);
            vTaskDelay(pdMS_TO_TICKS(700));
            const bool sent = prepared && uart.sendString(command);
            ESP_LOGI(TAG, "TX: %.*s (attempt %d/%d)",
                     static_cast<int>(std::strlen(command) - 2), command,
                     attempt, kModeCommandAttempts);
            const EventBits_t bits = sent
                                         ? xEventGroupWaitBits(
                                               self->events, ready, pdTRUE,
                                               pdFALSE, pdMS_TO_TICKS(2200))
                                         : 0;
            acknowledged = (bits & ready) != 0;
            if (acknowledged || self->requested_mode.load() != mode) break;
            ESP_LOGW(TAG,
                     "Bluetooth mode %d did not acknowledge attempt %d/%d",
                     static_cast<int>(mode), attempt, kModeCommandAttempts);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        self->mode_command_running.store(false);

        const ModuleMode requested = self->requested_mode.load();
        if (requested != mode) {
            self->StartMode(requested);
            vTaskDelete(nullptr);
            return;
        }

        if (!acknowledged) {
            if (mode == ModuleMode::Transmitter) {
                self->reset_reenable_pending.store(false);
                self->UseLocalRoute(true);
                self->StartMode(ModuleMode::Local);
            } else if (mode == ModuleMode::Local) {
                ESP_LOGE(TAG,
                         "Local Bluetooth I2S input is not ready; wake word remains paused");
            }
            vTaskDelete(nullptr);
            return;
        }

        if (mode == ModuleMode::Transmitter &&
            self->scan_after_mode_ready.exchange(false)) {
            self->Scan();
        }
        vTaskDelete(nullptr);
    }

    void StartMode(ModuleMode mode) {
        if (!initialized.load()) return;
        requested_mode.store(mode);
        bool expected = false;
        if (!mode_command_running.compare_exchange_strong(expected, true)) {
            ESP_LOGI(TAG, "Queued Bluetooth mode %d after current command",
                     static_cast<int>(mode));
            return;
        }
        auto* args = new ModeTaskArgs{this, mode};
        if (xTaskCreate(ModeTask, "bt_mode", 4096, args, 5, nullptr) !=
            pdPASS) {
            delete args;
            mode_command_running.store(false);
            ESP_LOGE(TAG, "Failed to create Bluetooth mode task");
        }
    }

    struct ProfileTaskArgs {
        Impl* self;
        AudioProfile profile;
    };

    static void ProfileTask(void* parameter) {
        auto* args = static_cast<ProfileTaskArgs*>(parameter);
        Impl* self = args->self;
        const AudioProfile profile = args->profile;
        delete args;
        SimpleUart& uart = SimpleUart::getInstance();
        self->audio_session.fetch_add(1);
        self->sco_connected.store(false);
        self->sco_input_ready.store(false);
        self->sco_validation_running.store(false);
        if (profile == AudioProfile::Call) {
            uart.sendString("AT+PP=1\r\n");
            vTaskDelay(pdMS_TO_TICKS(200));
            uart.sendString("AT+BTSCO=1\r\n");
            ESP_LOGI(TAG,
                     "Selected Bluetooth call profile; waiting for SETUP SCO");
        } else if (profile == AudioProfile::Music) {
            uart.sendString("AT+BTSCO=0\r\n");
            vTaskDelay(pdMS_TO_TICKS(200));
            uart.sendString("AT+PP=1\r\n");
            ESP_LOGI(TAG, "Selected Bluetooth music profile");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        self->profile_command_running.store(false);
        if (profile == AudioProfile::Music &&
            self->requested_profile.load() == profile) {
            self->ValidateAudioInput(false);
        }
        const AudioProfile requested = self->requested_profile.load();
        if (requested != AudioProfile::None && requested != profile) {
            self->StartProfileCommand(requested);
        }
        vTaskDelete(nullptr);
    }

    void StartProfileCommand(AudioProfile profile) {
        requested_profile.store(profile);
        bool expected = false;
        if (!profile_command_running.compare_exchange_strong(expected, true)) {
            ESP_LOGI(TAG, "Queued Bluetooth audio profile %d",
                     static_cast<int>(profile));
            return;
        }
        auto* args = new ProfileTaskArgs{this, profile};
        if (xTaskCreate(ProfileTask, "bt_profile", 4096, args, 5, nullptr) !=
            pdPASS) {
            delete args;
            profile_command_running.store(false);
            if (profile == AudioProfile::Music) {
                ValidateAudioInput(false);
            }
        }
    }

    static void ResetTask(void* parameter) {
        auto* self = static_cast<Impl*>(parameter);
        SimpleUart::getInstance().sendString("AT+CLEAR\r\n");
        ESP_LOGI(TAG, "TX: AT+CLEAR");
        vTaskDelay(pdMS_TO_TICKS(700));
        auto& io = IOExpander::getInstance();
        io.setLevel(IOExpander::Pin::BT_POWER, false);
        vTaskDelay(pdMS_TO_TICKS(300));
        io.setLevel(IOExpander::Pin::BT_POWER, true);
        vTaskDelay(pdMS_TO_TICKS(1200));
        self->reset_running.store(false);
        self->reset_reenable_pending.store(true);
        self->scan_after_mode_ready.store(true);
        self->StartMode(ModuleMode::Transmitter);
        vTaskDelete(nullptr);
    }

    void ResetModule() {
        bool expected = false;
        if (!reset_running.compare_exchange_strong(expected, true)) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.resetting = true;
        }
        UseLocalRoute(true);
        pending_device = {};
        ClearProfileState();
        Settings settings(kSettingsNamespace, true);
        settings.SetString(kRememberedAddressKey, "");
        settings.SetString(kRememberedNameKey, "");
        if (xTaskCreate(ResetTask, "bt_reset", 4096, this, 5, nullptr) !=
            pdPASS) {
            reset_running.store(false);
            {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.resetting = false;
            }
            Publish();
            StartMode(ModuleMode::Local);
        }
    }

    void SetEnabled(bool enabled) {
        if (enabled) {
            local_recovery_running.store(false);
            PauseWakeWord();
            ClearConnectionExpectations();
            ClearProfileState();
            {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.enabled = true;
                snapshot.scanning = false;
                snapshot.connection = ConnectionState::Idle;
                snapshot.has_current_device = false;
                snapshot.current_device = {};
            }
            Publish();
            StartMode(ModuleMode::Transmitter);
        } else {
            UseLocalRoute(true);
            StartMode(ModuleMode::Local);
        }
    }

    void Scan() {
        bool enabled = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            enabled = snapshot.enabled;
        }
        if (active_mode.load() != ModuleMode::Transmitter || !enabled) {
            return;
        }
        scan_expected.store(true);
        xEventGroupClearBits(events, kInquiryStarted);
        if (!SimpleUart::getInstance().sendString("AT+INQUIRING\r\n")) {
            scan_expected.store(false);
            return;
        }
        ESP_LOGI(TAG, "TX: AT+INQUIRING");
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.scanning = true;
            snapshot.connection = ConnectionState::Scanning;
            snapshot.nearby_devices.clear();
        }
        Publish();
    }

    void Connect(std::size_t index) {
        Device device;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (index >= snapshot.nearby_devices.size()) return;
            device = snapshot.nearby_devices[index];
        }
        if (!IsValidAddress(device.address)) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_device = device;
            snapshot.scanning = false;
            snapshot.connection = ConnectionState::Connecting;
        }
        scan_expected.store(false);
        ClearConnectionExpectations();
        manual_connect_expected.store(true);
        char command[48];
        std::snprintf(command, sizeof(command), "AT+CONNECT=%s\r\n",
                      device.address.c_str());
        if (!SimpleUart::getInstance().sendString(command)) {
            manual_connect_expected.store(false);
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.connection = ConnectionState::Idle;
        }
        Publish();
    }

    void SetAudioProfile(AudioProfile profile) {
        bool connected = false;
        bool unchanged = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = snapshot.connection == ConnectionState::Connected;
            unchanged = connected && snapshot.audio_profile == profile &&
                        requested_profile.load() == profile;
            if (connected && !unchanged) snapshot.audio_profile = profile;
        }
        if (!connected || profile == AudioProfile::None || unchanged) return;
        Publish();
        PauseWakeWord();
        AudioOutput_SetTarget(AudioOutputTarget::BluetoothSpeaker, false);
        StartProfileCommand(profile);
    }
};

Adapter& Adapter::Get() {
    static Adapter adapter;
    return adapter;
}

Adapter::Adapter() : impl_(new Impl) {}

Adapter::~Adapter() {
    AudioOutput_SetVolumeChangeHandler(nullptr, nullptr);
    delete impl_;
    impl_ = nullptr;
}

void Adapter::Initialize() {
    bool expected = false;
    if (!impl_->initialized.compare_exchange_strong(expected, true)) return;
    impl_->events = xEventGroupCreate();
    if (impl_->events == nullptr) {
        impl_->initialized.store(false);
        ESP_LOGE(TAG, "Failed to create Bluetooth event group");
        return;
    }
    impl_->LoadSettings();
    AudioOutput_SetVolumeChangeHandler(
        [](int volume, void* context) {
            auto* adapter = static_cast<Adapter*>(context);
            adapter->impl_->SetDeviceVolume(volume);
        },
        this);
    SimpleUart::getInstance().registerSystemCallback(
        [this](const std::vector<uint8_t>& data) {
            impl_->OnUartData(data);
        });
    AudioOutput_SetTarget(AudioOutputTarget::LocalSpeaker, false);
    impl_->StartMode(ModuleMode::Local);
}

void Adapter::SetEventSink(EventSink sink) {
    impl_->SetSink(std::move(sink));
}

bool Adapter::IsEnabled() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshot.enabled;
}

bool Adapter::IsConnected() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshot.connection == ConnectionState::Connected;
}

void Adapter::Execute(const Command& command) {
    switch (command.type) {
        case CommandType::Start:
            impl_->Publish();
            break;
        case CommandType::Stop:
            break;
        case CommandType::SetEnabled:
            impl_->SetEnabled(command.enabled);
            break;
        case CommandType::Scan:
            impl_->Scan();
            break;
        case CommandType::Connect:
            impl_->Connect(command.index);
            break;
        case CommandType::Reset:
            impl_->ResetModule();
            break;
        case CommandType::SetAudioProfile:
            impl_->SetAudioProfile(command.audio_profile);
            break;
    }
}

}  // namespace agent_ui::bluetooth
