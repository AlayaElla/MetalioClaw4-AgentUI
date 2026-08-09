#pragma once

#include <cstdint>

enum class AudioOutputTarget : uint8_t {
    LocalSpeaker = 0,
    BluetoothSpeaker = 1,
};

using AudioOutputVolumeChangeHandler = void (*)(int volume, void* context);

// Product-level audio route. It owns the persisted target and the local PA
// gate so UI pages, standby policy and the codec do not manipulate PA
// independently.
AudioOutputTarget AudioOutput_GetTarget();
void AudioOutput_SetTarget(AudioOutputTarget target, bool persist = true);
void AudioOutput_SetCodecEnabled(bool enabled);
void AudioOutput_SetStandby(bool standby);
void AudioOutput_SetVolumeChangeHandler(AudioOutputVolumeChangeHandler handler,
                                        void* context);
void AudioOutput_NotifyVolumeChanged(int volume);
