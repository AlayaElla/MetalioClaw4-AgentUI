#include "external_synth_renderer.h"

#include <algorithm>
#include <cmath>

namespace agent_ui::external_apps {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kMaxFrequencyHz = 7500.0f;
constexpr float kOutputGain = 0.88f;

float PolyBlep(float phase, float phase_step) {
    if (phase_step <= 0.0f) return 0.0f;
    if (phase < phase_step) {
        const float x = phase / phase_step;
        return x + x - x * x - 1.0f;
    }
    if (phase > 1.0f - phase_step) {
        const float x = (phase - 1.0f) / phase_step;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

uint32_t MillisecondsToSamples(uint16_t milliseconds) {
    return std::max<uint32_t>(1, static_cast<uint32_t>(milliseconds) *
                                     SynthRenderer::kSampleRate / 1000U);
}

}  // namespace

void SynthRenderer::Reset() {
    params_ = Params{};
    phase_ = 0.0f;
    vibrato_phase_ = 0.0f;
    frequency_hz_ = 440.0f;
    frequency_target_hz_ = 440.0f;
    frequency_step_ = 0.0f;
    frequency_remaining_ = 0;
    level_ = 0.0f;
    level_target_ = 0.0f;
    level_step_ = 0.0f;
    level_remaining_ = 0;
    filter_state_ = 0.0f;
    filter_alpha_ = 1.0f;
}

SynthRenderer::Params SynthRenderer::Convert(
        const metalio_app_synth_params_t& params) {
    Params converted;
    const uint32_t millihz = std::clamp<uint32_t>(
        params.frequency_millihz, 20000U, 7500000U);
    converted.frequency_hz = static_cast<float>(millihz) / 1000.0f;
    converted.level = static_cast<float>(
        std::min<uint16_t>(params.level_per_mille, 1000U)) / 1000.0f;
    converted.waveform = params.waveform <= METALIO_APP_SYNTH_THEREMIN
                            ? params.waveform
                            : static_cast<uint8_t>(METALIO_APP_SYNTH_SINE);
    converted.vibrato_cents = static_cast<float>(
        std::min<uint16_t>(params.vibrato_cents, 200U));
    converted.vibrato_hz = static_cast<float>(
        params.vibrato_centihz) / 100.0f;
    converted.brightness = static_cast<float>(
        std::min<uint16_t>(params.brightness_per_mille, 1000U)) / 1000.0f;
    if (converted.brightness >= 1.0f) {
        converted.filter_alpha = 1.0f;
    } else {
        const float cutoff_hz = 120.0f + converted.brightness *
                                            converted.brightness * 7600.0f;
        converted.filter_alpha =
            1.0f - std::exp(-kTwoPi * cutoff_hz /
                            static_cast<float>(kSampleRate));
    }
    converted.glide_samples =
        static_cast<uint32_t>(params.glide_ms) * kSampleRate / 1000U;
    converted.level_ramp_samples = std::max<uint32_t>(
        5U * kSampleRate / 1000U,
        static_cast<uint32_t>(params.level_ramp_ms) * kSampleRate / 1000U);
    return converted;
}

void SynthRenderer::SetTarget(const metalio_app_synth_params_t& params) {
    const Params converted = Convert(params);
    const bool frequency_changed = converted.frequency_hz != params_.frequency_hz;
    const bool level_changed = converted.level != params_.level;
    params_ = converted;
    filter_alpha_ = params_.filter_alpha;
    if (frequency_changed) {
        RampFrequency(params_.frequency_hz, params_.glide_samples);
    }
    if (level_changed) {
        RampLevel(params_.level, params_.level_ramp_samples);
    }
}

void SynthRenderer::RampToSilence(uint16_t ramp_ms) {
    RampLevel(0.0f, MillisecondsToSamples(ramp_ms));
}

void SynthRenderer::RampFrequency(float target_hz, uint32_t samples) {
    target_hz = std::clamp(target_hz, 20.0f, kMaxFrequencyHz);
    frequency_target_hz_ = target_hz;
    frequency_remaining_ = samples;
    if (samples == 0) {
        frequency_hz_ = target_hz;
        frequency_step_ = 0.0f;
    } else {
        frequency_step_ = (target_hz - frequency_hz_) /
                          static_cast<float>(samples);
    }
}

void SynthRenderer::RampLevel(float target_level, uint32_t samples) {
    target_level = std::clamp(target_level, 0.0f, 1.0f);
    level_target_ = target_level;
    level_remaining_ = samples;
    if (samples == 0) {
        level_ = target_level;
        level_step_ = 0.0f;
    } else {
        level_step_ = (target_level - level_) / static_cast<float>(samples);
    }
}

float SynthRenderer::Oscillator(float phase, float phase_step) const {
    switch (params_.waveform) {
        case METALIO_APP_SYNTH_TRIANGLE:
            return 1.0f - 4.0f * std::fabs(phase - 0.5f);
        case METALIO_APP_SYNTH_SAW:
            return 2.0f * phase - 1.0f - PolyBlep(phase, phase_step);
        case METALIO_APP_SYNTH_SQUARE: {
            const float square = phase < 0.5f ? 1.0f : -1.0f;
            return square + PolyBlep(phase, phase_step) -
                   PolyBlep(std::fmod(phase + 0.5f, 1.0f), phase_step);
        }
        case METALIO_APP_SYNTH_THEREMIN:
        {
            const float second_gain = std::clamp(
                (0.5f - 2.0f * phase_step) / 0.04f, 0.0f, 1.0f);
            const float third_gain = std::clamp(
                (0.5f - 3.0f * phase_step) / 0.04f, 0.0f, 1.0f);
            const float fundamental = std::sin(kTwoPi * phase);
            const float second = std::sin(kTwoPi * 2.0f * phase);
            const float third = std::sin(kTwoPi * 3.0f * phase);
            return (fundamental + 0.12f * second_gain * second +
                    0.06f * third_gain * third) /
                   (1.0f + 0.12f * second_gain + 0.06f * third_gain);
        }
        case METALIO_APP_SYNTH_SINE:
        default:
            return std::sin(kTwoPi * phase);
    }
}

float SynthRenderer::NextSample() {
    if (frequency_remaining_ > 0) {
        frequency_hz_ += frequency_step_;
        if (--frequency_remaining_ == 0) {
            frequency_hz_ = frequency_target_hz_;
            frequency_step_ = 0.0f;
        }
    }
    if (level_remaining_ > 0) {
        level_ += level_step_;
        if (--level_remaining_ == 0) {
            level_ = level_target_;
            level_step_ = 0.0f;
        }
    }

    float modulated_hz = frequency_hz_;
    if (params_.vibrato_cents > 0.0f && params_.vibrato_hz > 0.0f) {
        const float cents = params_.vibrato_cents * std::sin(vibrato_phase_);
        modulated_hz *= std::exp2(cents / 1200.0f);
        vibrato_phase_ += kTwoPi * params_.vibrato_hz /
                          static_cast<float>(kSampleRate);
        if (vibrato_phase_ >= kTwoPi) vibrato_phase_ -= kTwoPi;
    }
    modulated_hz = std::clamp(modulated_hz, 20.0f, kMaxFrequencyHz);
    const float phase_step = modulated_hz / static_cast<float>(kSampleRate);
    float sample = Oscillator(phase_, phase_step);
    phase_ += phase_step;
    if (phase_ >= 1.0f) phase_ -= std::floor(phase_);

    if (params_.brightness < 1.0f) {
        filter_state_ += filter_alpha_ * (sample - filter_state_);
        sample = filter_state_;
    } else {
        filter_state_ = sample;
    }

    return sample * level_ * kOutputGain;
}

void SynthRenderer::Render(int16_t* output, size_t sample_count) {
    if (output == nullptr) return;
    for (size_t index = 0; index < sample_count; ++index) {
        const float value = std::clamp(NextSample(), -1.0f, 1.0f);
        output[index] = static_cast<int16_t>(value * 32767.0f);
    }
}

}  // namespace agent_ui::external_apps
