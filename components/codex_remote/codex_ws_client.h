#ifndef CODEX_WS_CLIENT_H
#define CODEX_WS_CLIENT_H

#include <atomic>
#include <mutex>
#include <string>
#include <functional>
#include <vector>
#include <cstdint>
#include "esp_err.h"
#include "esp_websocket_client.h"
#include "codex_ws_transport_policy.h"

class CodexWsClient {
public:
    using MessageCallback = std::function<void(const std::string& message)>;
    using StatusCallback = std::function<void(bool connected)>;
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
    bool StartDiscovery(int timeout_ms = 5000, int initial_delay_ms = 0);
    void SetDiscoveryEnabled(bool enabled);
    void Disconnect();
    // Retry the current peer after an application-level failure. Do not call
    // this from a WebSocket event callback, because it stops and destroys the
    // active client before creating its replacement.
    bool Reconnect();

    // NVS 配置存取
    bool SaveToken(const std::string& token);
    bool LoadToken(std::string& out_token) const;
    bool HasToken() const;

    // 发送文本 JSON 控制消息
    bool SendTextMessage(const std::string& json_str, TickType_t timeout_ticks = portMAX_DELAY);

    // 发送二进制 Opus 语音数据帧
    bool SendOpusAudioFrame(const uint8_t* data, size_t length);
    // Realtime audio is explicitly bound to one request. The legacy binary
    // path is deliberately not reused because it has no session identity.
    bool SendRealtimeOpusAudioFrame(const std::string& request_id, uint32_t sequence,
                                    const uint8_t* data, size_t length);

    void SetOnMessageCallback(MessageCallback cb) { on_message_cb_ = cb; }
    void SetOnStatusCallback(StatusCallback cb) { on_status_cb_ = cb; }
    void SetOnDiscoveryCallback(DiscoveryCallback cb) { on_discovery_cb_ = cb; }

    bool IsConnected() const { return app_active_ && transport_state_.IsConnected(); }
    uint32_t GetConnectionGeneration() const { return transport_state_.Generation(); }
    std::string GetCurrentIp() const { return current_ip_; }
    int GetCurrentPort() const { return current_port_; }

private:
    CodexWsClient();
    ~CodexWsClient();

    static void EventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    static void DiscoveryTask(void* task_args);
    void NotifyConnected();
    void NotifyDisconnected();

    esp_websocket_client_handle_t client_handle_;
    codex_remote::transport::ConnectionState transport_state_;
    std::atomic<bool> discovery_running_;
    std::atomic<bool> app_active_{false};
    std::atomic<bool> discovery_enabled_{true};
    std::atomic<uint32_t> discovery_epoch_{0};
    std::recursive_mutex endpoint_mutex_;
    std::string current_ip_;
    std::string current_uri_;
    int current_port_;
    std::string rx_buffer_;
    std::string auth_header_;

    MessageCallback on_message_cb_;
    StatusCallback on_status_cb_;
    DiscoveryCallback on_discovery_cb_;
};

#endif // CODEX_WS_CLIENT_H
