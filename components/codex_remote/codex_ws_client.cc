#include "codex_ws_client.h"
#include "codex_ws_endpoint.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <algorithm>
#include <esp_log.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cstring>
#include <new>
#include <unistd.h>

extern "C" void codex_remote_handle_notification_json(const char* json);

static const char* TAG = "CodexWsClient";
static const char* NVS_NAMESPACE = "codex_remote";

namespace {
constexpr int kDiscoveryPort = 8766;
constexpr int kDiscoveryProtocolVersion = 1;
constexpr int kDiscoveryAttemptMs = 750;
constexpr char kDiscoveryRequest[] =
    "{\"type\":\"codex-remote-discovery\",\"protocolVersion\":1}";

struct DiscoveryTaskArgs {
    CodexWsClient* client;
    int timeout_ms;
    int initial_delay_ms;
    uint32_t epoch;
};
}

CodexWsClient& CodexWsClient::GetInstance() {
    static CodexWsClient instance;
    return instance;
}

CodexWsClient::CodexWsClient()
    : client_handle_(nullptr)
    , discovery_running_(false)
    , current_port_(8765) {
}

CodexWsClient::~CodexWsClient() {
    Disconnect();
}

esp_err_t CodexWsClient::Init() {
    ESP_LOGI(TAG, "Initializing Codex WebSocket Client");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

bool CodexWsClient::SaveToken(const std::string& token) {
    if (token.empty() || token.size() > 160) return false;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(nvs, "token", token.c_str());
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save PC config: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Saved Codex Remote authentication token");
    return true;
}

bool CodexWsClient::LoadToken(std::string& out_token) const {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    char token_buf[161] = {0};
    size_t required_size = sizeof(token_buf);
    err = nvs_get_str(nvs, "token", token_buf, &required_size);
    nvs_close(nvs);

    if (err == ESP_OK && token_buf[0] != '\0') {
        out_token.assign(token_buf);
        return true;
    }

    return false;
}

bool CodexWsClient::Connect(const std::string& ip, int port) {
    if (port < 1 || port > 65535) return false;
    return Connect(ip + ":" + std::to_string(port));
}

bool CodexWsClient::Connect(const std::string& address) {
    std::lock_guard<std::recursive_mutex> lock(endpoint_mutex_);
    if (!app_active_) return false;
    std::string uri, host;
    int port = 0;
    if (!codex_remote::ParseEndpoint(address, uri, host, port)) {
        ESP_LOGW(TAG, "Invalid server address: explicit port (1-65535) required");
        return false;
    }
    std::string token;
    if (!LoadToken(token)) {
        ESP_LOGW(TAG, "Authentication token is required before connecting");
        return false;
    }
    if (client_handle_) {
        Disconnect();
    }

    current_ip_ = host;
    current_uri_ = uri;
    current_port_ = port;
    rx_buffer_.clear();

    ESP_LOGI(TAG, "Connecting to WebSocket server: %s:%d", host.c_str(), port);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = current_uri_.c_str();
    if (uri.compare(0, 6, "wss://") == 0) ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ws_cfg.ping_interval_sec = codex_remote::transport::kPingIntervalSeconds;
    ws_cfg.pingpong_timeout_sec = codex_remote::transport::kPongTimeoutSeconds;
    ws_cfg.reconnect_timeout_ms = codex_remote::transport::kReconnectTimeoutMs;
    ws_cfg.network_timeout_ms = codex_remote::transport::kNetworkTimeoutMs;
    // Retain the same client for reconnects, including a clean bridge close.
    ws_cfg.enable_close_reconnect = true;
    auth_header_ = "Authorization: Bearer " + token + "\r\n";
    ws_cfg.headers = auth_header_.c_str();

    client_handle_ = esp_websocket_client_init(&ws_cfg);
    if (client_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return false;
    }

    esp_websocket_register_events(client_handle_, WEBSOCKET_EVENT_ANY, EventHandler, this);

    esp_err_t err = esp_websocket_client_start(client_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(client_handle_);
        client_handle_ = nullptr;
        return false;
    }

    return true;
}

bool CodexWsClient::HasToken() const {
    std::string token;
    return LoadToken(token);
}

bool CodexWsClient::StartDiscovery(int timeout_ms, int initial_delay_ms) {
    std::lock_guard<std::recursive_mutex> lock(endpoint_mutex_);
    if (!app_active_ || !discovery_enabled_) return false;
    if (transport_state_.IsConnected() || timeout_ms <= 0) return false;

    bool expected = false;
    if (!discovery_running_.compare_exchange_strong(expected, true)) return false;

    auto* args = new (std::nothrow) DiscoveryTaskArgs{
        this,
        timeout_ms,
        std::max(0, initial_delay_ms),
        discovery_epoch_.load()
    };
    if (!args) {
        discovery_running_ = false;
        return false;
    }

    if (xTaskCreate(DiscoveryTask, "codex_discovery", 4096, args, 4, nullptr) != pdPASS) {
        delete args;
        discovery_running_ = false;
        ESP_LOGE(TAG, "Failed to create LAN discovery task");
        return false;
    }

    return true;
}

void CodexWsClient::SetAppActive(bool active) {
    std::lock_guard<std::recursive_mutex> lock(endpoint_mutex_);
    if (app_active_ == active) return;
    app_active_ = active;
    ++discovery_epoch_;
    if (!active) Disconnect();
}

void CodexWsClient::SetDiscoveryEnabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(endpoint_mutex_);
    if (discovery_enabled_ == enabled) return;
    discovery_enabled_ = enabled;
    ++discovery_epoch_;
}

void CodexWsClient::DiscoveryTask(void* task_args) {
    auto* args = static_cast<DiscoveryTaskArgs*>(task_args);
    CodexWsClient* client = args->client;
    const int timeout_ms = args->timeout_ms;
    const int initial_delay_ms = args->initial_delay_ms;
    const uint32_t epoch = args->epoch;
    delete args;

    if (initial_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(initial_delay_ms));
    }

    int sock = -1;
    if (client->app_active_ && client->discovery_enabled_ &&
        client->discovery_epoch_ == epoch && !client->transport_state_.IsConnected()) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    }

    if (sock >= 0) {
        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        struct timeval receive_timeout = {};
        receive_timeout.tv_sec = 0;
        receive_timeout.tv_usec = kDiscoveryAttemptMs * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));

        sockaddr_in destination = {};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(kDiscoveryPort);
        destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        ESP_LOGI(TAG, "Searching for Codex Remote PC on UDP port %d", kDiscoveryPort);
        const TickType_t started_at = xTaskGetTickCount();
        const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

        while (client->app_active_ && client->discovery_enabled_ && client->discovery_epoch_ == epoch &&
               !client->transport_state_.IsConnected() &&
               xTaskGetTickCount() - started_at < timeout_ticks) {
            {
                std::lock_guard<std::recursive_mutex> lock(client->endpoint_mutex_);
                if (!client->app_active_ || !client->discovery_enabled_ || client->discovery_epoch_ != epoch) break;
                sendto(sock, kDiscoveryRequest, strlen(kDiscoveryRequest), 0,
                       reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
            }

            char response_buffer[384] = {0};
            sockaddr_in source = {};
            socklen_t source_length = sizeof(source);
            const int received = recvfrom(
                sock,
                response_buffer,
                sizeof(response_buffer) - 1,
                0,
                reinterpret_cast<sockaddr*>(&source),
                &source_length);
            if (received <= 0) continue;

            response_buffer[received] = '\0';
            cJSON* response = cJSON_Parse(response_buffer);
            if (!response) continue;

            const cJSON* type = cJSON_GetObjectItemCaseSensitive(response, "type");
            const cJSON* version = cJSON_GetObjectItemCaseSensitive(response, "protocolVersion");
            const cJSON* ws_port = cJSON_GetObjectItemCaseSensitive(response, "wsPort");
            const cJSON* name = cJSON_GetObjectItemCaseSensitive(response, "name");
            const bool valid = cJSON_IsString(type)
                && strcmp(type->valuestring, "codex-remote-discovery-response") == 0
                && cJSON_IsNumber(version)
                && version->valueint == kDiscoveryProtocolVersion
                && cJSON_IsNumber(ws_port)
                && ws_port->valueint >= 1024
                && ws_port->valueint <= 65535;
            const int discovered_port = valid ? ws_port->valueint : 0;
            const std::string discovered_name = cJSON_IsString(name)
                ? name->valuestring
                : "Codex Remote PC";
            cJSON_Delete(response);
            if (!valid) continue;

            char discovered_ip[INET_ADDRSTRLEN] = {0};
            if (!inet_ntop(AF_INET, &source.sin_addr, discovered_ip, sizeof(discovered_ip))) {
                continue;
            }

            std::lock_guard<std::recursive_mutex> lock(client->endpoint_mutex_);
            if (!client->app_active_ || !client->discovery_enabled_ || client->discovery_epoch_ != epoch) break;
            ESP_LOGI(TAG, "Discovered Codex Remote PC at %s:%d",
                     discovered_ip, discovered_port);
            client->current_ip_ = discovered_ip;
            client->current_port_ = discovered_port;
            if (client->on_discovery_cb_) {
                client->on_discovery_cb_(discovered_name, discovered_ip,
                                         discovered_port);
            }
            if (client->HasToken()) {
                client->Connect(discovered_ip, discovered_port);
            }
            break;
        }

        close(sock);
    } else if (client->app_active_ && client->discovery_epoch_ == epoch && !client->transport_state_.IsConnected()) {
        ESP_LOGW(TAG, "Could not create LAN discovery socket");
    }

    {
        std::lock_guard<std::recursive_mutex> lock(client->endpoint_mutex_);
        client->discovery_running_ = false;
        if (client->app_active_ && client->discovery_enabled_ && client->discovery_epoch_ != epoch) {
            client->StartDiscovery(timeout_ms);
        }
    }
    vTaskDelete(nullptr);
}

void CodexWsClient::Disconnect() {
    std::lock_guard<std::recursive_mutex> lock(endpoint_mutex_);
    // Clear the active handle before stop/destroy: an event from this explicit
    // shutdown is stale by definition and must not alter a later connection.
    esp_websocket_client_handle_t handle = client_handle_;
    client_handle_ = nullptr;
    NotifyDisconnected();
    rx_buffer_.clear();
    if (handle) {
        esp_websocket_client_stop(handle);
        esp_websocket_client_destroy(handle);
    }
}

bool CodexWsClient::Reconnect() {
    std::lock_guard<std::recursive_mutex> lock(endpoint_mutex_);
    if (!app_active_) return false;
    const bool has_endpoint = !current_ip_.empty() &&
        current_port_ >= 1 && current_port_ <= 65535;
    if (!codex_remote::transport::CanReconnect(has_endpoint, HasToken())) {
        ESP_LOGW(TAG, "Cannot reconnect without a saved peer and token");
        return false;
    }

    const std::string uri = current_uri_;
    return Connect(uri);
}

bool CodexWsClient::SendTextMessage(const std::string& json_str, TickType_t timeout_ticks) {
    std::lock_guard<std::recursive_mutex> lock(endpoint_mutex_);
    if (!app_active_ || !client_handle_ || !transport_state_.IsConnected()) {
        ESP_LOGE(TAG, "Cannot send text: Client not connected");
        return false;
    }

    int res = esp_websocket_client_send_text(client_handle_, json_str.c_str(), json_str.length(), timeout_ticks);
    return res == static_cast<int>(json_str.length());
}

bool CodexWsClient::SendOpusAudioFrame(const uint8_t* data, size_t length) {
    std::lock_guard<std::recursive_mutex> lock(endpoint_mutex_);
    if (!app_active_ || !client_handle_ || !transport_state_.IsConnected()) {
        ESP_LOGE(TAG, "Cannot send audio: Client not connected");
        return false;
    }

    int res = esp_websocket_client_send_bin(client_handle_, (const char*)data, length, portMAX_DELAY);
    return res >= 0;
}

bool CodexWsClient::SendRealtimeOpusAudioFrame(const std::string& request_id,
                                                uint32_t sequence,
                                                const uint8_t* data,
                                                size_t length) {
    constexpr size_t kMaxRawOpusBytes = 4096;
    constexpr size_t kMaxEncodedBytes = 5464;
    constexpr size_t kMaxRealtimeJsonBytes = 8192;
    if (request_id.empty() || data == nullptr || length == 0 || length > kMaxRawOpusBytes) {
        ESP_LOGW(TAG, "Rejecting invalid realtime audio frame");
        return false;
    }
    size_t encoded_length = 0;
    const int sizing = mbedtls_base64_encode(nullptr, 0, &encoded_length, data, length);
    if (sizing != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || encoded_length > kMaxEncodedBytes) return false;
    std::string encoded(encoded_length, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(),
                              &written, data, length) != 0) return false;
    encoded.resize(written);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "realtime_audio_input");
    cJSON_AddStringToObject(root, "requestId", request_id.c_str());
    cJSON_AddNumberToObject(root, "sequence", sequence);
    cJSON_AddNumberToObject(root, "generation", 1);
    cJSON_AddNumberToObject(root, "sampleRate", 16000);
    cJSON_AddNumberToObject(root, "frameDuration", 60);
    cJSON_AddStringToObject(root, "codec", "opus");
    cJSON_AddStringToObject(root, "data", encoded.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    const std::string message = printed != nullptr ? printed : "";
    if (printed != nullptr) cJSON_free(printed);
    cJSON_Delete(root);
    return !message.empty() && message.size() <= kMaxRealtimeJsonBytes && SendTextMessage(message);
}

void CodexWsClient::EventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    CodexWsClient* client = static_cast<CodexWsClient*>(handler_args);
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
    if (!client || !client->app_active_ || !data || data->client != client->client_handle_) {
        ESP_LOGW(TAG, "Ignoring WebSocket event from an inactive client");
        return;
    }

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            client->NotifyConnected();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
            client->NotifyDisconnected();
            break;

        case WEBSOCKET_EVENT_DATA:
            // A text frame can be delivered in several events by ESP-IDF.
            if (data->op_code == 0x01 && data->data_ptr && data->data_len > 0) {
                if (data->payload_offset == 0) client->rx_buffer_.clear();
                if (data->payload_len > 512 * 1024 || data->payload_offset != static_cast<int>(client->rx_buffer_.size()) ||
                    client->rx_buffer_.size() + data->data_len > 512 * 1024) {
                    client->rx_buffer_.clear();
                    break;
                }
                client->rx_buffer_.append(data->data_ptr, data->data_len);

                const int payload_length = data->payload_len > 0
                    ? data->payload_len
                    : static_cast<int>(client->rx_buffer_.size());
                if (data->payload_offset + data->data_len >= payload_length) {
                    ESP_LOGD(TAG, "Received text message (%u bytes)",
                             static_cast<unsigned>(client->rx_buffer_.size()));
                    codex_remote_handle_notification_json(client->rx_buffer_.c_str());
                    if (client->on_message_cb_) {
                        client->on_message_cb_(client->rx_buffer_);
                    }
                    client->rx_buffer_.clear();
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR: type=%d errno=%d",
                     static_cast<int>(data->error_handle.error_type),
                     data->error_handle.esp_transport_sock_errno);
            client->NotifyDisconnected();
            break;
    }
}

void CodexWsClient::NotifyConnected() {
    if (transport_state_.MarkConnected() && on_status_cb_) {
        on_status_cb_(true);
    }
}

void CodexWsClient::NotifyDisconnected() {
    rx_buffer_.clear();
    if (transport_state_.MarkDisconnected() && on_status_cb_) {
        on_status_cb_(false);
    }
}
