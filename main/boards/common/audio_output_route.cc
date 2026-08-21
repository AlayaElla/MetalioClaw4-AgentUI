#include "audio_output_route.h"

#include "IOExpander.hpp"
#include "esp_log.h"
#include "settings.h"

namespace {

constexpr const char* TAG = "AudioRoute";
constexpr const char* kNamespace = "audio";
constexpr const char* kTargetKey = "output_target";

bool s_loaded = false;
bool s_codec_enabled = false;
bool s_standby = false;
AudioOutputTarget s_target = AudioOutputTarget::LocalSpeaker;
AudioOutputVolumeChangeHandler s_volume_change_handler = nullptr;
void* s_volume_change_context = nullptr;
AudioOutputCodecChangeHandler s_codec_change_handler = nullptr;
void* s_codec_change_context = nullptr;

AudioOutputTarget NormalizeTarget(int value) {
    return value == static_cast<int>(AudioOutputTarget::BluetoothSpeaker)
               ? AudioOutputTarget::BluetoothSpeaker
               : AudioOutputTarget::LocalSpeaker;
}

void EnsureLoaded() {
    if (s_loaded) {
        return;
    }
    Settings settings(kNamespace, false);
    s_target = NormalizeTarget(settings.GetInt(
        kTargetKey, static_cast<int>(AudioOutputTarget::LocalSpeaker)));
    s_loaded = true;
}

void ApplyPaState() {
    EnsureLoaded();
    const bool enable_pa = s_target == AudioOutputTarget::LocalSpeaker &&
                           s_codec_enabled && !s_standby;
    IOExpander::getInstance().setLevel(IOExpander::Pin::PA, enable_pa);
    ESP_LOGI(TAG, "target=%s codec=%d standby=%d pa=%d",
             s_target == AudioOutputTarget::LocalSpeaker ? "local" : "bluetooth",
             s_codec_enabled ? 1 : 0, s_standby ? 1 : 0, enable_pa ? 1 : 0);
}

}  // namespace

AudioOutputTarget AudioOutput_GetTarget() {
    EnsureLoaded();
    return s_target;
}

void AudioOutput_SetTarget(AudioOutputTarget target, bool persist) {
    EnsureLoaded();
    s_target = target;
    if (persist) {
        Settings settings(kNamespace, true);
        settings.SetInt(kTargetKey, static_cast<int>(target));
    }
    ApplyPaState();
}

void AudioOutput_SetCodecEnabled(bool enabled) {
    s_codec_enabled = enabled;
    ApplyPaState();
    if (s_codec_change_handler != nullptr) {
        s_codec_change_handler(enabled, s_target, s_codec_change_context);
    }
}

void AudioOutput_SetStandby(bool standby) {
    s_standby = standby;
    ApplyPaState();
}

void AudioOutput_SetVolumeChangeHandler(AudioOutputVolumeChangeHandler handler,
                                        void* context) {
    s_volume_change_handler = handler;
    s_volume_change_context = context;
}

void AudioOutput_SetCodecChangeHandler(AudioOutputCodecChangeHandler handler,
                                       void* context) {
    s_codec_change_handler = handler;
    s_codec_change_context = context;
}

void AudioOutput_NotifyVolumeChanged(int volume) {
    if (s_volume_change_handler != nullptr) {
        s_volume_change_handler(volume, s_volume_change_context);
    }
}
