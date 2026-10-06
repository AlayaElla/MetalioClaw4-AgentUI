#pragma once

#include <cstddef>
#include <cstdint>

#include "metalio_app_api.h"

namespace agent_ui::external_apps {

// Small allocation-free 16 kHz mono synthesizer. Calls to SetTarget and
// Render are expected to be serialized by its owning audio worker.
class SynthRenderer {
public:
    static constexpr uint32_t kSampleRate = 16000;

    void Reset();
    void SetTarget(const metalio_app_synth_params_t& params);
    void RampToSilence(uint16_t ramp_ms);
    void Render(int16_t* output, size_t sample_count);

private:
    struct Params {
        float frequency_hz = 440.0f;
        float level = 0.0f;
        float vibrato_cents = 0.0f;
        float vibrato_hz = 0.0f;
        float brightness = 1.0f;
        float filter_alpha = 1.0f;
        uint8_t waveform = METALIO_APP_SYNTH_SINE;
        uint32_t glide_samples = 0;
        uint32_t level_ramp_samples = 80;
    };

    static Params Convert(const metalio_app_synth_params_t& params);
    float NextSample();
    float Oscillator(float phase, float phase_step) const;
    void RampFrequency(float target_hz, uint32_t samples);
    void RampLevel(float target_level, uint32_t samples);

    Params params_{};
    float phase_ = 0.0f;
    float vibrato_phase_ = 0.0f;
    float frequency_hz_ = 440.0f;
    float frequency_target_hz_ = 440.0f;
    float frequency_step_ = 0.0f;
    uint32_t frequency_remaining_ = 0;
    float level_ = 0.0f;
    float level_target_ = 0.0f;
    float level_step_ = 0.0f;
    uint32_t level_remaining_ = 0;
    float filter_state_ = 0.0f;
    float filter_alpha_ = 1.0f;
};

}  // namespace agent_ui::external_apps
