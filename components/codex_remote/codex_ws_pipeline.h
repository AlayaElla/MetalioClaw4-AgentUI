#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "cJSON.h"

namespace codex_remote::pipeline {

// The bridge emits compact device state capped at 448 KiB. Action-result
// envelopes can include that state, so the RX wire ceiling leaves room for
// the small control wrapper. DOM limits are preflight limits, before cJSON
// allocates. internal_preferred_bytes is diagnostic only: IDF's configured
// allocator reserve/fallback remains authoritative and is not a hard gate.
constexpr size_t kMaxWireBytes = 512 * 1024;
constexpr size_t kMaxFrameNodes = 24 * 1024;
constexpr size_t kMaxFrameDomBytes = 2 * 1024 * 1024;
constexpr size_t kMaxRetainedDomBytes = 4 * 1024 * 1024;
constexpr size_t kMaxRetainedNodes = 48 * 1024;
constexpr size_t kMaxRetainedMessages = 32;
constexpr size_t kMaxRealtimeAudioBytes = 4096;
constexpr size_t kMaxRealtimeAudioBase64Bytes = 5464;

struct Footprint {
    size_t nodes = 0;
    size_t strings = 0;
    size_t dom_bytes = 0;
    size_t internal_preferred_bytes = 0;
    size_t max_depth = 0;
};

enum class CheckResult : uint8_t {
    Ok,
    Empty,
    TooLarge,
    TooDeep,
    TooManyNodes,
    DomLimit,
    InternalLimit,
    InvalidJson,
};

struct ReservationStats {
    size_t bytes = 0;
    size_t nodes = 0;
    size_t internal_preferred_bytes = 0;
    size_t high_water_bytes = 0;
    size_t high_water_nodes = 0;
    size_t messages = 0;
    size_t high_water_messages = 0;
    size_t rejected = 0;
};

class RetainedBudget {
public:
    bool TryReserve(const Footprint& footprint);
    void Release(const Footprint& footprint);
    void ReleaseFootprint(const Footprint& footprint);
    ReservationStats GetStats() const;

private:
    mutable std::mutex mutex_;
    ReservationStats stats_{};
};

class Reservation {
public:
    Reservation() = default;
    Reservation(RetainedBudget* budget, const Footprint& footprint);
    ~Reservation();
    Reservation(Reservation&& other) noexcept;
    Reservation& operator=(Reservation&& other) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

    void Reset();
    const Footprint& footprint() const { return footprint_; }
    bool ShrinkTo(const Footprint& footprint);

private:
    RetainedBudget* budget_ = nullptr;
    Footprint footprint_{};
};

struct ParsedJson {
    cJSON* root = nullptr;
    Footprint footprint{};
    Reservation reservation{};
    ParsedJson() = default;
    ~ParsedJson();
    ParsedJson(ParsedJson&& other) noexcept;
    ParsedJson& operator=(ParsedJson&& other) noexcept;
    ParsedJson(const ParsedJson&) = delete;
    ParsedJson& operator=(const ParsedJson&) = delete;
};

using ParseFunction = cJSON* (*)(const char* data, size_t length);

CheckResult EstimateJson(std::string_view json, Footprint* out);
CheckResult MeasureDom(const cJSON* root, Footprint* out);
CheckResult ParseBoundedJson(std::string_view json, RetainedBudget* budget,
                             ParsedJson* out,
                             ParseFunction parse = nullptr);

// Token-range helpers for preserving small ACK envelopes when an optional
// top-level state subtree exceeds the ESP display budget. They recognize the
// JSON structure while respecting escaped strings; they do not use regex.
bool ReadTopLevelStringMember(std::string_view json, std::string_view key,
                              std::string_view* value);
bool ElideTopLevelMember(std::string_view json, std::string_view key,
                         std::string* output);

// Strict, bounded base64 decoding used by the realtime-audio receive path.
bool DecodeBase64(std::string_view encoded, size_t max_output_bytes,
                  std::vector<uint8_t>* output);

}  // namespace codex_remote::pipeline
