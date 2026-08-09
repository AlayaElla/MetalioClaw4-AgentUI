#ifndef CODEX_WS_CLIENT_H
#define CODEX_WS_CLIENT_H

#include <atomic>
#include <string>
#include <functional>
#include <vector>
#include <cstdint>
#include "esp_err.h"
#include "esp_websocket_client.h"

class CodexWsClient {
public:
    using MessageCallback = std::function<void(const std::string& message)>;
    using StatusCallback = std::function<void(bool connected)>;
    using DiscoveryCallback = std::function<void(const std::string& name,
                                                  const std::string& ip,
                                                  int port)>;

    static CodexWsClient& GetInstance();

    esp_err_t Init();
    bool Connect(const std::string& ip, int port = 8765);
    bool StartDiscovery(int timeout_ms = 5000, int initial_delay_ms = 0);
    void Disconnect();

    // NVS 配置存取
    bool SaveToken(const std::string& token);
    bool LoadToken(std::string& out_token) const;
    bool HasToken() const;

    // 发送文本 JSON 控制消息
    bool SendTextMessage(const std::string& json_str);

    // 发送二进制 Opus 语音数据帧
    bool SendOpusAudioFrame(const uint8_t* data, size_t length);

    void SetOnMessageCallback(MessageCallback cb) { on_message_cb_ = cb; }
    void SetOnStatusCallback(StatusCallback cb) { on_status_cb_ = cb; }
    void SetOnDiscoveryCallback(DiscoveryCallback cb) { on_discovery_cb_ = cb; }

    bool IsConnected() const { return connected_.load(); }
    std::string GetCurrentIp() const { return current_ip_; }

private:
    CodexWsClient();
    ~CodexWsClient();

    static void EventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    static void DiscoveryTask(void* task_args);

    esp_websocket_client_handle_t client_handle_;
    std::atomic<bool> connected_;
    std::atomic<bool> discovery_running_;
    std::string current_ip_;
    int current_port_;
    std::string rx_buffer_;
    std::string auth_header_;

    MessageCallback on_message_cb_;
    StatusCallback on_status_cb_;
    DiscoveryCallback on_discovery_cb_;
};

#endif // CODEX_WS_CLIENT_H
