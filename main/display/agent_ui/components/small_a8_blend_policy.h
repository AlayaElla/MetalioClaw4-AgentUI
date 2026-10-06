#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace agent_ui {

// Compare real draws of similar sizes, rather than benchmark twice into the
// same framebuffer. Neither path changes the animation cadence or dirty area.
class SmallA8BlendPolicy {
public:
    static constexpr uint32_t kPixelLimit = 4096;
    static constexpr uint32_t kWarmupSamples = 8;
    static constexpr uint32_t kProbeInterval = 128;

    struct Samples {
        uint32_t ppa_count = 0;
        uint32_t software_count = 0;
        uint64_t ppa_us = 0;
        uint64_t software_us = 0;
        uint32_t decisions = 0;
        bool ppa_failed = false;
    };

    bool SelectPpa(uint32_t pixels, bool rgb888, uint32_t cpu_hz) {
        if (cpu_hz != cpu_hz_) {
            samples_ = {};
            cpu_hz_ = cpu_hz;
        }
        if (pixels >= kPixelLimit) return true;
        auto& sample = samples_[Index(pixels, rgb888)];
        ++sample.decisions;
        const bool probe = sample.decisions % kProbeInterval == 0;
        if (sample.ppa_failed) return probe;
        if (sample.ppa_count < kWarmupSamples || sample.software_count < kWarmupSamples) {
            return sample.ppa_count <= sample.software_count;
        }
        // Require a meaningful margin before preferring software. Probe the
        // other backend occasionally so load changes cannot lock in a choice.
        const bool prefer_ppa = sample.software_us * 5 >= sample.ppa_us * 4;
        return probe ? !prefer_ppa : prefer_ppa;
    }

    void Record(uint32_t pixels, bool rgb888, bool ppa, uint64_t elapsed_us) {
        if (pixels >= kPixelLimit) return;
        auto& sample = samples_[Index(pixels, rgb888)];
        auto& count = ppa ? sample.ppa_count : sample.software_count;
        auto& mean = ppa ? sample.ppa_us : sample.software_us;
        if (elapsed_us == 0) elapsed_us = 1;
        if (count < kWarmupSamples) {
            mean = (mean * count + elapsed_us) / (count + 1);
            ++count;
        } else {
            mean = (mean * 7 + elapsed_us) / 8;
        }
        if (ppa) sample.ppa_failed = false;
    }

    void PpaFailed(uint32_t pixels, bool rgb888) {
        if (pixels < kPixelLimit) samples_[Index(pixels, rgb888)].ppa_failed = true;
    }

    const Samples& Get(uint32_t pixels, bool rgb888) const {
        return samples_[Index(pixels, rgb888)];
    }

private:
    static size_t Index(uint32_t pixels, bool rgb888) {
        const size_t bucket = pixels <= 64 ? 0 : pixels <= 256 ? 1 : pixels <= 1024 ? 2 : 3;
        return bucket + (rgb888 ? 4 : 0);
    }
    std::array<Samples, 8> samples_{};
    uint32_t cpu_hz_ = 0;
};

}  // namespace agent_ui
