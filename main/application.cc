#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "codex_ws_client.h"
#include "settings.h"

#include <algorithm>
#include <ctime>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#include <ssid_manager.h>
#include <inttypes.h>

#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#endif

#ifdef HAVE_LVGL
#include "agent_ui/agent_ui_runtime.h"
#include "agent_ui/apps/home/home_renderer.h"
#include "agent_ui/core/idle_power.h"
#include "agent_ui/core/navigation.h"
#include "agent_ui/core/status_bar.h"
#include "esp_lv_adapter.h"
#endif

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

namespace {

std::string SpecialInteractionPrompt(SpecialInteraction interaction, int detail) {
    switch (interaction) {
        case SpecialInteraction::Charging:
            if (detail >= 0) {
                return "(接通电源，电量" +
                       std::to_string(std::clamp(detail, 0, 100)) + "%)";
            }
            return "(接通电源)";
        case SpecialInteraction::Sleep:
        {
            const std::time_t now = std::time(nullptr);
            std::tm local = {};
            char time_text[6] = "--:--";
            if (localtime_r(&now, &local) != nullptr && local.tm_year >= 125) {
                std::strftime(time_text, sizeof(time_text), "%H:%M", &local);
            }
            return std::string("(准备进入待机 ") + time_text + ")";
        }
        case SpecialInteraction::Dizzy:
            if (detail > 0) {
                return "(用户摇晃了你" + std::to_string(detail) + "次)";
            }
            return "(用户摇晃了你)";
        case SpecialInteraction::None:
            return {};
    }
    return {};
}

}  // namespace

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();


    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::ProvisionDevice(ProvisioningClient& client) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

        esp_err_t err = client.FetchConfiguration();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, provisioning failed");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, client.GetEndpointUrl().c_str());
            ESP_LOGW(TAG, "Provisioning failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (!client.HasActivationCode() && !client.HasActivationChallenge()) {
            break;
        }

        while (activation_suspended_) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (client.HasActivationCode()) {
            ShowActivationCode(client.GetActivationCode(), client.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            while (activation_suspended_) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = client.Activate();
            if (err == ESP_OK) {
                pending_activation_code_.clear();
#ifdef HAVE_LVGL
                agent_ui::StatusBar::Get().RefreshAsync();
#endif
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    if (activation_suspended_) {
        return;
    }

    pending_activation_code_ = code;
#ifdef HAVE_LVGL
    agent_ui::StatusBar::Get().RefreshAsync();
#endif

    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        if (activation_suspended_) {
            return;
        }
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::SetActivationSuspended(bool suspended) {
    activation_suspended_ = suspended;
    if (suspended) {
        DismissAlert();
        ESP_LOGI(TAG, "Activation suspended for stress test");
    } else {
        ESP_LOGI(TAG, "Activation resumed after stress test");
    }
}

void Application::StopSystemAudioForStressTest() {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }

    if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (device_state_ == kDeviceStateListening && protocol_) {
        protocol_->SendStopListening();
    }

    audio_service_.EnableAudioTesting(false);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.ResetDecoder();

    for (int i = 0; i < 20 && !audio_service_.IsIdle(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (device_state_ == kDeviceStateListening ||
        device_state_ == kDeviceStateSpeaking ||
        device_state_ == kDeviceStateConnecting) {
        SetDeviceState(kDeviceStateIdle);
    }

    DismissAlert();
    ESP_LOGI(TAG, "System audio stopped for stress test");
}

void Application::RestoreSystemAudioAfterStressTest() {
    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EnableWakeWordDetection(true);
    }
    ESP_LOGI(TAG, "System audio restored after stress test");
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty() && !activation_suspended_) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (low_power_standby_.load()) return;
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (low_power_standby_.load()) return;
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (low_power_standby_.load()) return;
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (low_power_standby_.load()) return;
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::StartCodexVoiceCapture() {
    Schedule([this]() {
        if (low_power_standby_.load()) return;
        if (codex_voice_capture_active_ || codex_voice_start_pending_) return;
        codex_voice_stop_pending_ = false;
        codex_voice_restore_wake_word_ = false;
        codex_voice_stopped_callback_ = {};
        codex_voice_start_pending_ = true;
        TryStartCodexVoiceCapture();
    });
}

void Application::StopCodexVoiceCapture(std::function<void()> on_stopped) {
    Schedule([this, on_stopped = std::move(on_stopped)]() mutable {
        if (codex_voice_start_pending_) {
            codex_voice_start_pending_ = false;
            if (on_stopped) on_stopped();
            return;
        }
        if (!codex_voice_capture_active_) {
            if (on_stopped) on_stopped();
            return;
        }

        audio_service_.EnableVoiceProcessing(false);
        codex_voice_stop_pending_ = true;
        codex_voice_stopped_callback_ = std::move(on_stopped);
        TryFinishCodexVoiceCapture();
    });
}

void Application::TryStartCodexVoiceCapture() {
    if (low_power_standby_.load()) {
        codex_voice_start_pending_ = false;
        return;
    }
    if (!codex_voice_start_pending_ || audio_service_.HasPendingSendAudio()) return;

    codex_voice_start_pending_ = false;
    codex_voice_capture_active_ = true;
    codex_voice_restore_wake_word_ = audio_service_.IsWakeWordRunning();
    if (codex_voice_restore_wake_word_) {
        audio_service_.EnableWakeWordDetection(false);
    }
    audio_service_.EnableVoiceProcessing(true);
    ESP_LOGI(TAG, "Codex voice capture started");
}

void Application::TryFinishCodexVoiceCapture() {
    if (!codex_voice_stop_pending_ || audio_service_.HasPendingSendAudio()) return;

    codex_voice_capture_active_ = false;
    codex_voice_stop_pending_ = false;
    if (codex_voice_restore_wake_word_ &&
        device_state_ == kDeviceStateIdle && !low_power_standby_.load()) {
        audio_service_.EnableWakeWordDetection(true);
    }
    codex_voice_restore_wake_word_ = false;
    auto callback = std::move(codex_voice_stopped_callback_);
    codex_voice_stopped_callback_ = {};
    ESP_LOGI(TAG, "Codex voice capture stopped");
    if (callback) callback();
}

void Application::Start() {
    auto& board = Board::GetInstance();
#ifdef HAVE_LVGL
    // Board::GetInstance() has returned, so its singleton is fully constructed.
    // Runtime power management is then started asynchronously on the LVGL task.
    agent_ui::Runtime::Get().OnBoardReady(board);
#endif
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // ESP_LOGI(TAG, "---测试OTA地址---");
    // auto &ssid_manager = SsidManager::GetInstance();
    // ssid_manager.AddSsid("CloudZao-RJ", "asdfghjkl");
    // ESP_LOGI(TAG, "---测试OTA地址---");

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    ProvisioningClient provisioning;

// #if CONFIG_ESP_HOSTED_ENABLED
//     /* Boot-time probe: C5 ESP-Hosted slave (WiFi coprocessor) present? */
//     ESP_LOGI(TAG, "C5 hosted check: connecting to slave...");
//     if (esp_hosted_connect_to_slave() == ESP_OK) {
//         esp_hosted_coprocessor_fwver_t fwver{};
//         uint32_t chip_id = 0;
//         char target_name[32] = {0};
//         if (esp_hosted_get_coprocessor_fwversion(&fwver) == ESP_OK) {
//             ESP_LOGI(TAG,
//                      "C5 hosted check: OK — FW %" PRIu32 ".%" PRIu32 ".%" PRIu32
//                      " (rev=%" PRId32 ")",
//                      fwver.major1, fwver.minor1, fwver.patch1, fwver.revision);
//         } else {
//             ESP_LOGW(TAG, "C5 hosted check: transport up, but fwversion RPC failed");
//         }
//         if (esp_hosted_get_cp_info(&chip_id, target_name, sizeof(target_name)) == ESP_OK) {
//             ESP_LOGI(TAG, "C5 hosted check: chip_id=0x%" PRIx32 " target=%s",
//                      chip_id, target_name[0] ? target_name : "(n/a)");
//         }
//     } else {
//         ESP_LOGE(TAG,
//                  "C5 hosted check: FAIL — slave not reachable "
//                  "(no hosted FW / SDIO / reset?)");
//         PlaySound(Lang::Sounds::OGG_ERR_REG);
//     }
// #endif

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

   
    // Check for new assets version
    // CheckAssetsVersion();

    ProvisionDevice(provisioning);
    //加载唤醒词模型
    GetAudioService().SetModelsList(esp_srmodel_init("model"));
    GetAudioService().EnableWakeWordDetection(false);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    if (provisioning.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (provisioning.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in provisioning configuration, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
            FinishSpecialInteraction(true);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (active_special_interaction_ != SpecialInteraction::None ||
                            listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                    if (active_special_interaction_ != SpecialInteraction::None) {
                        FinishSpecialInteraction(true);
                        if (protocol_ && protocol_->IsAudioChannelOpened()) {
                            protocol_->CloseAudioChannel();
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                if (active_special_interaction_ != SpecialInteraction::None) {
                    // The server echoes listen/detect text as STT. It is an
                    // internal action cue, not a visible user chat message.
                    ESP_LOGD(TAG, "Special interaction STT echo hidden");
                } else {
                    ESP_LOGI(TAG, ">> %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("user", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Honor an explicit server-requested reboot.
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = provisioning.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + esp_app_get_description()->version;
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        // audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            FinishSpecialInteraction(true);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (codex_voice_capture_active_) {
                    if (!CodexWsClient::GetInstance().SendOpusAudioFrame(
                            packet->payload.data(), packet->payload.size())) {
                        ESP_LOGW(TAG, "Failed to send Codex voice audio frame");
                        break;
                    }
                } else if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
            TryStartCodexVoiceCapture();
            TryFinishCodexVoiceCapture();
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (low_power_standby_.load() || !protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected("Hi 钛灵");
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (low_power_standby_.load() &&
        (state == kDeviceStateConnecting || state == kDeviceStateListening ||
         state == kDeviceStateSpeaking)) {
        ESP_LOGD(TAG, "AI state ignored during low-power standby");
        return;
    }
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(!low_power_standby_.load());
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }

}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // 重启前关背光，避免过渡花屏/蓝屏；不写 NVS，下次启动仍按原亮度恢复。
    if (Backlight* bl = Board::GetInstance().GetBacklight()) {
        bl->SetBrightness(0, false);
    }
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    // 等待背光渐暗（SetBrightness 约 5ms/级）后再重启
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (low_power_standby_.load() || !protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    if (sound.empty() || activation_suspended_) {
        return;
    }
    audio_service_.PlaySound(sound);
}

void Application::TriggerSpecialInteraction(SpecialInteraction interaction, int detail) {
    Schedule([this, interaction, detail]() {
        if (low_power_standby_.load()) {
            ESP_LOGD(TAG, "Special interaction skipped during standby");
            return;
        }
#ifdef HAVE_LVGL
        if (agent_ui::Navigation::Get().current() != agent_ui::ScreenId::Home) {
            ESP_LOGD(TAG, "Special interaction skipped outside Home");
            return;
        }
#endif
        const std::string prompt = SpecialInteractionPrompt(interaction, detail);
        if (prompt.empty() ||
            active_special_interaction_ != SpecialInteraction::None) {
            return;
        }
        if (device_state_ != kDeviceStateIdle || protocol_ == nullptr) {
            ESP_LOGD(TAG, "Special interaction skipped: AI is not idle");
            return;
        }
        if (!Board::GetInstance().IsNetworkConnected()) {
            ESP_LOGD(TAG, "Special interaction skipped: network is offline");
            return;
        }

        active_special_interaction_ = interaction;
#ifdef HAVE_LVGL
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (interaction == SpecialInteraction::Charging) {
                agent_ui::home::Renderer::HoldChargingExpression();
            } else if (interaction == SpecialInteraction::Dizzy) {
                agent_ui::home::Renderer::HoldDizzyExpression();
            }
            esp_lv_adapter_unlock();
        }
#endif
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                SetDeviceState(kDeviceStateIdle);
                FinishSpecialInteraction(true);
                return;
            }
        }

        listening_mode_ = kListeningModeManualStop;
        // Short detect replies can start delivering TTS audio immediately.
        // Prepare the decoder before sending so the first packets are not
        // discarded while the deferred tts/start state update is pending.
        SetDeviceState(kDeviceStateSpeaking);
        auto send_interaction = [this, interaction, prompt](
                                    std::unique_ptr<AudioStreamPacket> prime_packet) mutable {
            auto prime_packet_holder =
                std::make_shared<std::unique_ptr<AudioStreamPacket>>(std::move(prime_packet));
            Schedule([this, interaction, prompt = std::move(prompt),
                      prime_packet_holder]() mutable {
                if (active_special_interaction_ != interaction || protocol_ == nullptr) {
                    return;
                }
                if (!protocol_->PrimeAudioChannel(std::move(*prime_packet_holder))) {
                    ESP_LOGW(TAG, "Failed to prime special interaction audio channel");
                    if (protocol_->IsAudioChannelOpened()) {
                        protocol_->CloseAudioChannel();
                    }
                    SetDeviceState(kDeviceStateIdle);
                    FinishSpecialInteraction(true);
                    return;
                }
                if (!protocol_->SendTextInput(prompt)) {
                    ESP_LOGW(TAG, "Failed to send special interaction text");
                    if (protocol_->IsAudioChannelOpened()) {
                        protocol_->CloseAudioChannel();
                    }
                    SetDeviceState(kDeviceStateIdle);
                    FinishSpecialInteraction(true);
                    return;
                }
                ESP_LOGI(TAG, "Special interaction started: %d",
                         static_cast<int>(interaction));
            });
        };

        if (protocol_->RequiresAudioChannelPrime()) {
            constexpr size_t kSamplesPerFrame =
                16000 * OPUS_FRAME_DURATION_MS / 1000;
            audio_service_.EncodeAudio(
                std::vector<int16_t>(kSamplesPerFrame, 0),
                std::move(send_interaction));
        } else {
            send_interaction(nullptr);
        }
    });
}

void Application::FinishSpecialInteraction(bool restore_sleep) {
    const SpecialInteraction completed = active_special_interaction_;
    active_special_interaction_ = SpecialInteraction::None;

#ifdef HAVE_LVGL
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        agent_ui::home::Renderer::ReleaseSpecialExpression();
        if (restore_sleep && completed == SpecialInteraction::Sleep) {
            agent_ui::IdlePower::Get().RestoreExpressionSleep();
        }
        esp_lv_adapter_unlock();
    }
#endif
}

void Application::CancelSpecialInteraction() {
    FinishSpecialInteraction(false);
}

void Application::ForceReturnToIdle() {
    Schedule([this]() {
        const bool has_special_interaction =
            active_special_interaction_ != SpecialInteraction::None;
        if (!has_special_interaction &&
            device_state_ != kDeviceStateConnecting &&
            device_state_ != kDeviceStateListening &&
            device_state_ != kDeviceStateSpeaking) {
            return;
        }

        CancelSpecialInteraction();

        if (device_state_ == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone);
        } else if (device_state_ == kDeviceStateListening && protocol_) {
            protocol_->SendStopListening();
        }

        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        audio_service_.ResetDecoder();
        SetDeviceState(kDeviceStateIdle);
    });
}

void Application::SetLowPowerStandby(bool enabled) {
    const bool previous = low_power_standby_.exchange(enabled);
    if (previous == enabled) return;

    if (enabled) {
        standby_restore_wake_word_ = audio_service_.IsWakeWordRunning();
        audio_service_.EnableAudioTesting(false);
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.ResetDecoder();
        if (AudioCodec* codec = Board::GetInstance().GetAudioCodec()) {
            codec->EnableInput(false);
        }

        Schedule([this]() {
            codex_voice_start_pending_ = false;
            if (codex_voice_capture_active_) {
                codex_voice_stop_pending_ = true;
                TryFinishCodexVoiceCapture();
            }
            CancelSpecialInteraction();
            if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonNone);
            } else if (device_state_ == kDeviceStateListening && protocol_) {
                protocol_->SendStopListening();
            }
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->CloseAudioChannel();
            }
            if (device_state_ == kDeviceStateConnecting ||
                device_state_ == kDeviceStateListening ||
                device_state_ == kDeviceStateSpeaking) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
        ESP_LOGI(TAG, "AI audio suspended for low-power standby");
        return;
    }

    if (standby_restore_wake_word_ && device_state_ == kDeviceStateIdle) {
        audio_service_.EnableWakeWordDetection(true);
    }
    standby_restore_wake_word_ = false;
    ESP_LOGI(TAG, "AI audio resumed after low-power standby");
}
