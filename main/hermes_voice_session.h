#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "ai_provider_config.h"

namespace hermes_voice {

constexpr size_t kSampleRate = 16000;
constexpr size_t kFrameSamples = 960;
constexpr size_t kMaxRecordingSeconds = 30;
constexpr size_t kMaxRecordingBytes =
    kMaxRecordingSeconds * kSampleRate * sizeof(int16_t);
constexpr size_t kMaxHttpResponseBytes = 512 * 1024;
// A 30 s mono PCM16 WAV wrapped in base64 is roughly 1.28 MiB. Keep room for
// the JSON envelope while retaining a hard bound on outbound allocations.
constexpr size_t kMaxHttpRequestBytes = 2 * 1024 * 1024;
constexpr size_t kMaxTtsBytes = 320 * 1024;
constexpr size_t kMaxDecodedTtsSamples = 16000 * 60;

enum class State { Idle, Recording, Transcribing, Responding, Synthesizing, Speaking };

struct DataUrlResult {
    std::string mime_type;
    std::vector<uint8_t> bytes;
};

struct WavPcmResult { std::vector<int16_t> samples; };

// These pure helpers deliberately accept only bounded data. They are also
// host-testable without Wi-Fi, the board or an audio codec.
std::string BuildWavDataUrl(const std::vector<int16_t>& pcm);
bool ParseDataUrl(std::string_view value, size_t max_bytes, DataUrlResult* out);
bool ParseTtsDataUrlJson(std::string_view json, std::string* data_url);
bool ParseSttTranscriptJson(std::string_view json, std::string* transcript);
bool ParsePcmWav(std::string_view wav, WavPcmResult* out);
bool DecodeMpegToPcm16k(std::string_view mpeg, WavPcmResult* out);
bool ParseDashboardProfiles(std::string_view json, std::vector<std::string>* profiles);
bool ParsePasswordProvider(std::string_view json, std::string* provider);
bool ParseWsTicket(std::string_view json, std::string* ticket);
bool BuildCookieHeader(const std::vector<std::string>& set_cookie_headers,
                       std::string* cookie_header);
std::string DashboardWebSocketUrl(std::string_view base_url,
                                  std::string_view ticket,
                                  std::string_view profile);

class Session {
public:
    Session();
    ~Session();
    uint32_t StartRecording();
    // Returns true only when the dedicated endpoint VAD changes state.
    bool CopyProcessedFrame(const int16_t* samples, size_t count);
    std::vector<int16_t> StopRecording(uint32_t epoch);
    uint32_t Cancel();
    bool IsCurrent(uint32_t epoch) const;
    bool IsRecording() const { return recording_.load(std::memory_order_acquire); }
    bool IsEndpointVoiceDetected() const {
        return endpoint_voice_detected_.load(std::memory_order_acquire);
    }
    bool HasEndpointVad() const { return endpoint_vad_ != nullptr; }
    uint32_t endpoint_transitions() const {
        return endpoint_transitions_.load(std::memory_order_acquire);
    }
    uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }
    void SetState(State state) { state_.store(state, std::memory_order_release); }
    State state() const { return state_.load(std::memory_order_acquire); }

private:
    std::atomic<uint32_t> epoch_{0};
    std::atomic<bool> recording_{false};
    std::atomic<bool> endpoint_voice_detected_{false};
    std::atomic<uint32_t> endpoint_transitions_{0};
    std::atomic<State> state_{State::Idle};
    std::mutex recorder_mutex_;
    std::vector<int16_t> recorder_;
    void* endpoint_vad_ = nullptr;
};

struct HttpResult {
    int status = 0;
    std::string body;
    std::vector<std::string> set_cookie_headers;
};

struct DashboardSession {
    std::string cookie_header;
};

struct GatewayTurnResult {
    std::string stored_session_id;
    std::string text;
};

bool HttpJson(std::string_view method, std::string_view url,
              std::string_view cookie_header, std::string&& body,
              HttpResult* result, size_t max_response_bytes = kMaxHttpResponseBytes);
bool LoginDashboard(const AiProviderConfig& config, DashboardSession* session,
                    std::string* error);
bool CheckDashboardIdentity(const AiProviderConfig& config,
                            const DashboardSession& session, std::string* error);
bool LoadDashboardProfiles(const AiProviderConfig& config,
                           const DashboardSession& session,
                           std::vector<std::string>* profiles, std::string* error);
bool MintDashboardWsTicket(const AiProviderConfig& config,
                           const DashboardSession& session,
                           std::string* ticket, std::string* error);
bool TestDashboardGateway(const AiProviderConfig& config,
                          const DashboardSession& session, std::string* error);
bool RunDashboardTurn(const AiProviderConfig& config,
                      const DashboardSession& session,
                      std::string_view user_text,
                      std::string_view stored_session_id,
                      GatewayTurnResult* result, std::string* error,
                      std::function<bool()> keep_waiting = {});

}  // namespace hermes_voice
