#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace audio {

// The audio task publishes a latest-value snapshot.  The display timer is the
// only consumer of the onset bit, so a pulse cannot be replayed by subsequent
// frames.  No LVGL or task dispatching is involved in this boundary.
struct ListeningAudioFeatures {
    float activity = 0.0f;
    bool speech = false;
    bool onset = false;
};

class ListeningAudioFeatureStore {
public:
    ListeningAudioFeatureStore() noexcept { Clear(); }

    void PublishActivity(float activity, bool speech) noexcept {
        activity_bits_.store(FloatToBits(ClampActivity(activity)),
                              std::memory_order_release);
        speech_.store(speech, std::memory_order_release);
    }

    void SetSpeech(bool speech) noexcept {
        speech_.store(speech, std::memory_order_release);
    }

    void MarkOnset() noexcept { onset_.store(true, std::memory_order_release); }

    void ClearOnset() noexcept { onset_.store(false, std::memory_order_release); }

    ListeningAudioFeatures ReadLatest() noexcept {
        ListeningAudioFeatures features;
        features.activity = BitsToFloat(
            activity_bits_.load(std::memory_order_acquire));
        features.speech = speech_.load(std::memory_order_acquire);
        features.onset = onset_.exchange(false, std::memory_order_acq_rel);
        return features;
    }

    void Clear() noexcept {
        activity_bits_.store(FloatToBits(0.0f), std::memory_order_release);
        speech_.store(false, std::memory_order_release);
        onset_.store(false, std::memory_order_release);
    }

private:
    static float ClampActivity(float value) noexcept {
        if (!(value > 0.0f)) return 0.0f;
        if (value >= 1.0f) return 1.0f;
        return value;
    }

    static uint32_t FloatToBits(float value) noexcept {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    static float BitsToFloat(uint32_t bits) noexcept {
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::atomic<uint32_t> activity_bits_{0};
    std::atomic<bool> speech_{false};
    std::atomic<bool> onset_{false};
};

}  // namespace audio
