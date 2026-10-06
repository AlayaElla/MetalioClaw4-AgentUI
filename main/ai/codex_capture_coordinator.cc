#include "codex_capture_coordinator.h"

#include <utility>

namespace ai {

bool CodexCaptureCoordinator::SameContext(const TransportContext& left,
                                          const TransportContext& right) {
    return left.valid == right.valid &&
        left.app_generation == right.app_generation &&
        left.connection_generation == right.connection_generation &&
        left.connection_epoch == right.connection_epoch;
}

void CodexCaptureCoordinator::SetCaptureActive(bool active) {
    capture_active_.store(active, std::memory_order_release);
    capture_occupies_audio_.store(active || start_pending_, std::memory_order_release);
}

void CodexCaptureCoordinator::ReleaseAiBlock() {
    const uint64_t token = ai_block_token_;
    ai_block_token_ = 0;
    if (token != 0) port_.ReleaseAiBlock(token);
}

void CodexCaptureCoordinator::StartVoice(const TransportContext& context) {
    if (transitioning_ || port_.IsStandby() || IsCaptureActive() || start_pending_ ||
        !port_.IsAiAvailable() || !context.valid ||
        !port_.IsTransportCurrent(context)) return;

    ++capture_generation_;
    transport_context_ = context;
    mode_ = Mode::Voice;
    ai_block_token_ = port_.AcquireAiBlock();
    stop_pending_ = false;
    stop_wait_started_at_us_ = 0;
    voice_restore_wake_word_ = false;
    stopped_callback_ = {};
    start_pending_ = true;
    capture_occupies_audio_.store(true, std::memory_order_release);
    start_wait_started_at_us_ = port_.NowUs();
    TryStart();
}

void CodexCaptureCoordinator::StartRealtime(
    const std::string& request_id, const TransportContext& context) {
    if (transitioning_ || request_id.empty() || port_.IsStandby() || IsCaptureActive() ||
        start_pending_ || !context.valid ||
        !port_.IsTransportCurrent(context)) return;

    ++capture_generation_;
    transport_context_ = context;
    mode_ = Mode::Realtime;
    if (realtime_request_id_ != request_id) {
        realtime_request_id_ = request_id;
        realtime_audio_sequence_ = 0;
        realtime_restore_wake_word_ = port_.IsWakeWordRunning();
        if (realtime_restore_wake_word_) port_.EnableWakeWordDetection(false);
    }
    stop_pending_ = false;
    stop_wait_started_at_us_ = 0;
    voice_restore_wake_word_ = false;
    stopped_callback_ = {};
    start_pending_ = true;
    capture_occupies_audio_.store(true, std::memory_order_release);
    start_wait_started_at_us_ = port_.NowUs();
    TryStart();
}

void CodexCaptureCoordinator::StopVoice(std::function<void()> on_stopped) {
    if (start_pending_) {
        start_pending_ = false;
        start_wait_started_at_us_ = 0;
        transport_context_ = {};
        mode_ = Mode::None;
        capture_occupies_audio_.store(false, std::memory_order_release);
        ReleaseAiBlock();
        if (on_stopped) on_stopped();
        return;
    }
    if (!IsCaptureActive()) {
        if (on_stopped) on_stopped();
        return;
    }

    port_.EnableVoiceProcessing(false);
    stop_pending_ = true;
    stop_wait_started_at_us_ = port_.NowUs();
    stopped_callback_ = std::move(on_stopped);
    TryFinishVoice();
}

void CodexCaptureCoordinator::StopRealtime() {
    if (transitioning_) return;
    transitioning_ = true;
    const bool was_active = IsCaptureActive();
    const bool restore_wake_word = voice_restore_wake_word_;
    start_pending_ = false;
    start_wait_started_at_us_ = 0;
    SetCaptureActive(false);
    capture_occupies_audio_.store(false, std::memory_order_release);
    stop_pending_ = false;
    stop_wait_started_at_us_ = 0;
    [[maybe_unused]] auto abandoned_callback = std::move(stopped_callback_);
    stopped_callback_ = {};
    voice_restore_wake_word_ = false;
    transport_context_ = {};
    mode_ = Mode::None;
    const uint64_t ai_token = ai_block_token_;
    ai_block_token_ = 0;
    if (was_active) {
        port_.EnableVoiceProcessing(false);
        port_.CancelPendingSendAudio();
    }
    if (ai_token != 0) port_.ReleaseAiBlock(ai_token);
    if (restore_wake_word && port_.IsDeviceIdle() && !port_.IsStandby()) {
        port_.EnableWakeWordDetection(true);
    }
    transitioning_ = false;
}

void CodexCaptureCoordinator::EndRealtime(std::function<void()> on_stopped) {
    if (transitioning_) return;
    transitioning_ = true;
    const bool was_active = IsCaptureActive();
    const bool restore_voice_wake_word = voice_restore_wake_word_;
    const bool restore_realtime_wake_word = realtime_restore_wake_word_;
    const uint64_t ai_token = ai_block_token_;
    ai_block_token_ = 0;
    start_pending_ = false;
    start_wait_started_at_us_ = 0;
    SetCaptureActive(false);
    stop_pending_ = false;
    stop_wait_started_at_us_ = 0;
    [[maybe_unused]] auto abandoned_callback = std::move(stopped_callback_);
    stopped_callback_ = {};
    voice_restore_wake_word_ = false;
    realtime_restore_wake_word_ = false;
    transport_context_ = {};
    mode_ = Mode::None;
    realtime_request_id_.clear();
    realtime_audio_sequence_ = 0;
    capture_occupies_audio_.store(false, std::memory_order_release);
    if (was_active) {
        port_.EnableVoiceProcessing(false);
        port_.CancelPendingSendAudio();
    }
    if ((restore_voice_wake_word || restore_realtime_wake_word) &&
        port_.IsDeviceIdle() && !port_.IsStandby()) {
        port_.EnableWakeWordDetection(true);
    }
    port_.ClearRealtimePlayback();
    if (ai_token != 0) port_.ReleaseAiBlock(ai_token);
    transitioning_ = false;
    // State, request identity, context, and availability lease are finalized
    // before invoking code that may synchronously start a replacement session.
    if (on_stopped) on_stopped();
}

bool CodexCaptureCoordinator::OnSendAudio(const uint8_t* data, size_t size) {
    if (!IsCaptureActive() || mode_ == Mode::None) return false;
    const TransportContext context = transport_context_;
    const uint32_t generation = capture_generation_;
    const std::string request_id = realtime_request_id_;
    if (!context.valid || !port_.IsTransportCurrent(context)) {
        FailTransport(context);
        return true;
    }

    auto completion = [this, context, generation, request_id](const SendResult& result) {
        if (result.sent || !SameContext(result.context, context) ||
            !port_.IsTransportCurrent(context)) return;
        port_.PostToMain([this, context, generation, request_id]() {
            if (!port_.IsTransportCurrent(context) ||
                capture_generation_ != generation ||
                realtime_request_id_ != request_id) return;
            FailTransport(context);
        });
    };

    const Admission admission = mode_ == Mode::Voice
        ? port_.QueueVoiceFrame(data, size, context, std::move(completion))
        : request_id.empty()
            ? Admission::Rejected
            : port_.QueueRealtimeFrame(request_id, ++realtime_audio_sequence_,
                                       data, size, context, std::move(completion));
    if (admission != Admission::Accepted) FailTransport(context);
    return true;
}

void CodexCaptureCoordinator::OnAudioQueueChanged() {
    TryStart();
    TryFinishVoice();
}

void CodexCaptureCoordinator::TryStart() {
    if (!start_pending_) return;
    if (port_.IsStandby()) {
        start_pending_ = false;
        start_wait_started_at_us_ = 0;
        capture_occupies_audio_.store(false, std::memory_order_release);
        transport_context_ = {};
        mode_ = Mode::None;
        ReleaseAiBlock();
        return;
    }
    if (!transport_context_.valid || !port_.IsTransportCurrent(transport_context_)) {
        const TransportContext context = transport_context_;
        FailTransport(context);
        return;
    }
    if (port_.InterruptActiveSynth()) return;

    if (port_.HasPendingSendAudio()) {
        const int64_t now_us = port_.NowUs();
        if (start_wait_started_at_us_ == 0) start_wait_started_at_us_ = now_us;
        if (now_us - start_wait_started_at_us_ >= kQueueDrainTimeoutUs) {
            const TransportContext context = transport_context_;
            // This start has not acquired the shared audio queue. Leave those
            // packets untouched and release the request through its transport.
            transitioning_ = true;
            const bool same_transport = port_.IsTransportCurrent(context);
            start_pending_ = false;
            start_wait_started_at_us_ = 0;
            capture_occupies_audio_.store(false, std::memory_order_release);
            transport_context_ = {};
            mode_ = Mode::None;
            const uint64_t ai_token = ai_block_token_;
            ai_block_token_ = 0;
            if (same_transport) port_.RequestReconnect(context);
            if (ai_token != 0) port_.ReleaseAiBlock(ai_token);
            transitioning_ = false;
        }
        return;
    }

    start_pending_ = false;
    start_wait_started_at_us_ = 0;
    SetCaptureActive(true);
    voice_restore_wake_word_ = port_.IsWakeWordRunning();
    if (voice_restore_wake_word_) port_.EnableWakeWordDetection(false);
    port_.EnableVoiceProcessing(true);
}

void CodexCaptureCoordinator::TryFinishVoice() {
    if (!stop_pending_) return;
    bool cancel_audio = false;
    if (port_.HasPendingSendAudio()) {
        const int64_t now_us = port_.NowUs();
        if (stop_wait_started_at_us_ == 0) stop_wait_started_at_us_ = now_us;
        if (now_us - stop_wait_started_at_us_ < kQueueDrainTimeoutUs) return;
        cancel_audio = true;
    }

    transitioning_ = true;
    auto callback = std::move(stopped_callback_);
    stopped_callback_ = {};
    SetCaptureActive(false);
    const bool restore_wake_word = voice_restore_wake_word_;
    voice_restore_wake_word_ = false;
    const uint64_t ai_token = ai_block_token_;
    ai_block_token_ = 0;
    stop_pending_ = false;
    stop_wait_started_at_us_ = 0;
    transport_context_ = {};
    mode_ = Mode::None;
    if (cancel_audio) port_.CancelPendingSendAudio();
    if (restore_wake_word && port_.IsDeviceIdle() && !port_.IsStandby()) {
        port_.EnableWakeWordDetection(true);
    }
    if (ai_token != 0) port_.ReleaseAiBlock(ai_token);
    transitioning_ = false;
    if (callback) callback();
}

void CodexCaptureCoordinator::FailTransport(const TransportContext& context) {
    auto callback = std::move(stopped_callback_);
    stopped_callback_ = {};
    transitioning_ = true;
    const bool same_transport = context.valid && port_.IsTransportCurrent(context);
    ++capture_generation_;
    start_pending_ = false;
    start_wait_started_at_us_ = 0;
    const bool was_active = IsCaptureActive();
    SetCaptureActive(false);
    stop_pending_ = false;
    stop_wait_started_at_us_ = 0;
    const bool restore_voice_wake = voice_restore_wake_word_;
    voice_restore_wake_word_ = false;
    const bool restore_realtime_wake = realtime_restore_wake_word_;
    realtime_restore_wake_word_ = false;
    const bool had_realtime_session = !realtime_request_id_.empty();
    realtime_request_id_.clear();
    realtime_audio_sequence_ = 0;
    transport_context_ = {};
    mode_ = Mode::None;
    const uint64_t ai_token = ai_block_token_;
    ai_block_token_ = 0;
    if (was_active) {
        port_.EnableVoiceProcessing(false);
        port_.CancelPendingSendAudio();
    }
    if (had_realtime_session) port_.ClearRealtimePlayback();
    if ((restore_voice_wake || restore_realtime_wake) &&
        port_.IsDeviceIdle() && !port_.IsStandby()) {
        port_.EnableWakeWordDetection(true);
    }
    if (same_transport) port_.RequestReconnect(context);
    if (ai_token != 0) port_.ReleaseAiBlock(ai_token);
    transitioning_ = false;
    if (callback) callback();
}

void CodexCaptureCoordinator::AbortForStandby() {
    start_pending_ = false;
    start_wait_started_at_us_ = 0;
    capture_occupies_audio_.store(IsCaptureActive(), std::memory_order_release);
    if (IsCaptureActive()) {
        stop_pending_ = true;
        stop_wait_started_at_us_ = port_.NowUs();
        TryFinishVoice();
    } else {
        ReleaseAiBlock();
        transport_context_ = {};
        mode_ = Mode::None;
    }
}

}  // namespace ai
