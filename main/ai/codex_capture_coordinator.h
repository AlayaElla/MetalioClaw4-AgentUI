#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace ai {

// Owns the Codex microphone capture and realtime-uplink lifecycle. All methods
// that mutate lifecycle state are called on Application's main event loop;
// the two atomic queries are safe for the audio/UI policy readers.
class CodexCaptureCoordinator {
public:
    struct TransportContext {
        uint32_t app_generation = 0;
        uint32_t connection_generation = 0;
        uint32_t connection_epoch = 0;
        bool valid = false;
    };

    struct SendResult {
        bool sent = false;
        TransportContext context;
    };

    enum class Admission : uint8_t { Accepted, Rejected };

    class Port {
    public:
        using SendCompletion = std::function<void(const SendResult&)>;
        virtual ~Port() = default;

        virtual int64_t NowUs() const = 0;
        virtual bool IsStandby() const = 0;
        virtual bool IsDeviceIdle() const = 0;
        virtual bool IsAiAvailable() const = 0;
        virtual bool IsTransportCurrent(const TransportContext& context) const = 0;
        virtual bool IsWakeWordRunning() const = 0;
        virtual bool InterruptActiveSynth() = 0;
        virtual bool HasPendingSendAudio() const = 0;
        virtual void EnableWakeWordDetection(bool enabled) = 0;
        virtual void EnableVoiceProcessing(bool enabled) = 0;
        virtual void CancelPendingSendAudio() = 0;
        virtual uint64_t AcquireAiBlock() = 0;
        virtual void ReleaseAiBlock(uint64_t token) = 0;
        virtual void ClearRealtimePlayback() = 0;
        virtual void RequestReconnect(const TransportContext& context) = 0;
        // Transport completion runs off the main event loop. The coordinator
        // uses this hook to return the generation-checked failure to its owner.
        virtual void PostToMain(std::function<void()> callback) = 0;
        virtual Admission QueueVoiceFrame(const uint8_t* data, size_t size,
                                          const TransportContext& context,
                                          SendCompletion completion) = 0;
        virtual Admission QueueRealtimeFrame(const std::string& request_id,
                                             uint32_t sequence,
                                             const uint8_t* data, size_t size,
                                             const TransportContext& context,
                                             SendCompletion completion) = 0;
    };

    explicit CodexCaptureCoordinator(Port& port) : port_(port) {}
    CodexCaptureCoordinator(const CodexCaptureCoordinator&) = delete;
    CodexCaptureCoordinator& operator=(const CodexCaptureCoordinator&) = delete;

    void StartVoice(const TransportContext& context);
    void StartRealtime(const std::string& request_id,
                       const TransportContext& context);
    void StopVoice(std::function<void()> on_stopped = {});
    void StopRealtime();
    void EndRealtime(std::function<void()> on_stopped = {});

    // Application pops the shared AudioService queue once and passes each
    // packet here. Returns true when this packet belongs to Codex capture.
    bool OnSendAudio(const uint8_t* data, size_t size);
    void OnAudioQueueChanged();
    void AbortForStandby();

    bool IsCaptureActive() const {
        return capture_active_.load(std::memory_order_acquire);
    }
    bool IsCapturePendingOrActive() const {
        return capture_occupies_audio_.load(std::memory_order_acquire);
    }
    uint32_t CaptureGeneration() const { return capture_generation_; }
    const TransportContext& CaptureTransport() const { return transport_context_; }

private:
    enum class Mode : uint8_t { None, Voice, Realtime };

    static constexpr int64_t kQueueDrainTimeoutUs = 2 * 1000 * 1000;

    void TryStart();
    void TryFinishVoice();
    void FailTransport(const TransportContext& context);
    void ReleaseAiBlock();
    void SetCaptureActive(bool active);
    static bool SameContext(const TransportContext& left,
                            const TransportContext& right);

    Port& port_;
    std::atomic<bool> capture_active_{false};
    std::atomic<bool> capture_occupies_audio_{false};
    bool start_pending_ = false;
    int64_t start_wait_started_at_us_ = 0;
    bool stop_pending_ = false;
    int64_t stop_wait_started_at_us_ = 0;
    bool transitioning_ = false;
    bool voice_restore_wake_word_ = false;
    bool realtime_restore_wake_word_ = false;
    uint32_t capture_generation_ = 0;
    TransportContext transport_context_{};
    Mode mode_ = Mode::None;
    std::string realtime_request_id_;
    uint32_t realtime_audio_sequence_ = 0;
    std::function<void()> stopped_callback_;
    uint64_t ai_block_token_ = 0;
};

}  // namespace ai
