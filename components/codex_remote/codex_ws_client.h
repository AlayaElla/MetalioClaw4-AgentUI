#ifndef CODEX_WS_CLIENT_H
#define CODEX_WS_CLIENT_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <functional>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstdint>
#include "esp_err.h"
#include "esp_websocket_client.h"
#include "codex_ws_transport_policy.h"
#include "codex_ws_pipeline.h"

struct CodexWsMessage {
    enum class Notice : uint8_t { None, StateTooLarge };
    cJSON* root = nullptr;
    uint32_t app_generation = 0;
    uint32_t connection_generation = 0;
    uint32_t connection_epoch = 0;
    std::vector<uint8_t> realtime_audio;
    bool realtime_audio_valid = false;
    bool state_omitted = false;
    Notice notice = Notice::None;
    size_t retained_bytes = 0;
    codex_remote::pipeline::Reservation reservation{};

    ~CodexWsMessage();
    const cJSON* Json() const { return root; }
};

class CodexWsClient {
public:
    using MessageCallback = std::function<bool(std::shared_ptr<const CodexWsMessage> message)>;
    struct SendResult {
        bool sent = false;
        uint32_t app_generation = 0;
        uint32_t connection_generation = 0;
        uint32_t connection_epoch = 0;
    };
    struct SendContext {
        uint32_t app_generation = 0;
        uint32_t connection_generation = 0;
        uint32_t connection_epoch = 0;
        bool valid = false;
    };
    using StatusCallback = std::function<void(bool connected, uint32_t app_generation,
                                              uint32_t connection_generation,
                                              uint32_t connection_epoch)>;
    using DiscoveryCallback = std::function<void(const std::string& name,
                                                  const std::string& ip,
                                                  int port)>;

    static CodexWsClient& GetInstance();

    esp_err_t Init();
    // Only the foreground Codex view owns the transport. Deactivation also
    // cancels discovery and destroys the reconnecting WebSocket client.
    void SetAppActive(bool active);
    bool Connect(const std::string& address);
    bool Connect(const std::string& ip, int port);
    bool RequestConnectAsync(const std::string& address);
    bool StartDiscovery(int timeout_ms = 5000, int initial_delay_ms = 0);
    void SetDiscoveryEnabled(bool enabled);
    void Disconnect();
    // Retry the current peer after an application-level failure. Do not call
    // this from a WebSocket event callback, because it stops and destroys the
    // active client before creating its replacement.
    bool Reconnect();
    bool RequestReconnectAsync();
    // Retry only if this exact connection still owns the failure. A newer
    // explicit connect/disconnect invalidates the queued request.
    bool RequestReconnectAsync(const SendContext& context);

    // NVS 配置存取
    bool SaveToken(const std::string& token);
    bool LoadToken(std::string& out_token) const;
    bool HasToken() const;

    // 发送文本 JSON 控制消息
    bool SendTextMessage(const std::string& json_str, TickType_t timeout_ticks = portMAX_DELAY);
    enum class SendAdmission : uint8_t { Accepted, Inactive, Disconnected, Stale, TooLarge, Full, Unavailable };
    using SendCompletion = std::function<void(const SendResult& result)>;
    SendAdmission QueueTextMessage(std::string json_str, SendCompletion completion = {});
    SendAdmission QueueTextMessage(std::string json_str, const SendContext& context,
                                   SendCompletion completion = {});
    SendAdmission QueueOpusAudioFrame(const uint8_t* data, size_t length,
                                      SendCompletion completion = {});
    SendAdmission QueueOpusAudioFrame(const uint8_t* data, size_t length,
                                      const SendContext& context,
                                      SendCompletion completion = {});
    SendAdmission QueueRealtimeOpusAudioFrame(const std::string& request_id,
                                             uint32_t sequence, const uint8_t* data,
                                             size_t length, SendCompletion completion = {});
    SendAdmission QueueRealtimeOpusAudioFrame(const std::string& request_id,
                                             uint32_t sequence, const uint8_t* data,
                                             size_t length, const SendContext& context,
                                             SendCompletion completion = {});
    SendContext CaptureSendContext() const;

    // 发送二进制 Opus 语音数据帧
    bool SendOpusAudioFrame(const uint8_t* data, size_t length);
    // Realtime audio is explicitly bound to one request. The legacy binary
    // path is deliberately not reused because it has no session identity.
    bool SendRealtimeOpusAudioFrame(const std::string& request_id, uint32_t sequence,
                                    const uint8_t* data, size_t length);

    void SetOnMessageCallback(MessageCallback cb);
    void SetOnStatusCallback(StatusCallback cb);
    void SetOnDiscoveryCallback(DiscoveryCallback cb);

    bool IsConnected() const { return app_active_ && transport_state_.IsConnected(); }
    uint32_t GetConnectionGeneration() const { return transport_state_.Generation(); }
    uint32_t GetConnectionEpoch() const { return explicit_connection_epoch_.load(std::memory_order_acquire); }
    std::string GetCurrentIp() const {
        std::lock_guard<std::mutex> lock(endpoint_info_mutex_);
        return current_ip_;
    }
    int GetCurrentPort() const {
        std::lock_guard<std::mutex> lock(endpoint_info_mutex_);
        return current_port_;
    }
    uint32_t GetAppSessionGeneration() const { return app_session_generation_.load(std::memory_order_acquire); }
    bool IsAppActive() const { return app_active_.load(std::memory_order_acquire); }

private:
    struct RawMessage;
    struct TxMessage;
    struct PendingReconnectRequest {
        bool context_bound = false;
        bool claim_valid = false;
        uint32_t app_generation = 0;
        uint32_t connection_generation = 0;
        uint32_t claimed_epoch = 0;
    };
    enum class TxKind : uint8_t;

    CodexWsClient();
    ~CodexWsClient();

    static void EventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    static void DiscoveryTask(void* task_args);
    static void PipelineTask(void* task_args);
    static void TxTask(void* task_args);
    bool EnsurePipeline();
    void DisconnectWorker();
    SendAdmission QueuePayload(std::string payload, TxKind kind,
                               const SendContext& context, SendCompletion completion);
    void ProcessReceivedFrame(RawMessage* raw);
    void ProcessQueuedSend(TxMessage* tx);
    void ReclaimMessage(CodexWsMessage* message);
    void MaybeLogPipelineStats();
    void SignalRxOverflow();
    void RequestReconnectFromPipeline();
    void ClearRxAssembly();
    void NotifyConnected();
    void NotifyDisconnected();

    esp_websocket_client_handle_t client_handle_;
    codex_remote::transport::ConnectionState transport_state_;
    std::atomic<bool> discovery_running_;
    std::atomic<bool> app_active_{false};
    std::atomic<uint32_t> app_session_generation_{0};
    std::atomic<uint32_t> explicit_connection_epoch_{0};
    std::atomic<bool> discovery_enabled_{true};
    std::atomic<uint32_t> discovery_epoch_{0};
    std::recursive_mutex endpoint_mutex_;
    mutable std::mutex endpoint_info_mutex_;
    std::string current_ip_;
    std::string current_uri_;
    int current_port_;
    std::mutex rx_assembly_mutex_;
    uint32_t rx_app_generation_ = 0;
    uint32_t rx_connection_generation_ = 0;
    uint32_t rx_explicit_epoch_ = 0;
    size_t rx_expected_size_ = 0;
    size_t rx_buffer_size_ = 0;
    std::unique_ptr<uint8_t[]> rx_buffer_;
    std::string auth_header_;

    QueueHandle_t rx_queue_ = nullptr;
    QueueHandle_t tx_queue_ = nullptr;
    QueueHandle_t reclaim_queue_ = nullptr;
    QueueSetHandle_t pipeline_queue_set_ = nullptr;
    TaskHandle_t pipeline_task_ = nullptr;
    TaskHandle_t tx_task_ = nullptr;
    std::atomic<size_t> rx_queued_bytes_{0};
    std::atomic<size_t> tx_queued_bytes_{0};
    std::atomic<size_t> rx_queued_bytes_peak_{0};
    std::atomic<size_t> tx_queued_bytes_peak_{0};
    std::atomic<size_t> rx_queued_items_peak_{0};
    std::atomic<size_t> tx_queued_items_peak_{0};
    std::atomic<size_t> rx_assembly_bytes_{0};
    std::atomic<size_t> rx_assembly_bytes_peak_{0};
    std::atomic<size_t> rx_inflight_bytes_{0};
    std::atomic<size_t> rx_inflight_bytes_peak_{0};
    std::atomic<uint32_t> rx_overflow_events_{0};
    std::atomic<uint32_t> tx_rejected_events_{0};
    std::atomic<uint32_t> reclaim_enqueue_errors_{0};
    std::atomic<bool> rx_reconnect_requested_{false};
    std::atomic<bool> endpoint_disconnect_requested_{false};
    std::atomic<bool> endpoint_reconnect_requested_{false};
    std::atomic<bool> endpoint_connect_requested_{false};
    std::mutex endpoint_request_mutex_;
    std::string pending_connect_address_;
    PendingReconnectRequest pending_reconnect_request_{};
    std::atomic<esp_websocket_client_handle_t> callback_client_handle_{nullptr};
    size_t deferred_rx_events_ = 0;  // Pipeline task owns this counter.
    std::mutex pipeline_setup_mutex_;
    bool pipeline_started_ = false;
    codex_remote::pipeline::RetainedBudget retained_dom_budget_;
    std::mutex callback_mutex_;

    MessageCallback on_message_cb_;
    StatusCallback on_status_cb_;
    DiscoveryCallback on_discovery_cb_;
};

#endif // CODEX_WS_CLIENT_H
