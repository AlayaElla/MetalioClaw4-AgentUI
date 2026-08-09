#include "codex_ws_client.h"
#include "cJSON.h"
#include <algorithm>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cstring>
#include <new>
#include <unistd.h>

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
};
}

CodexWsClient& CodexWsClient::GetInstance() {
    static CodexWsClient instance;
    return instance;
}

CodexWsClient::CodexWsClient()
    : client_handle_(nullptr)
    , connected_(false)
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
    std::string token;
    if (!LoadToken(token)) {
        ESP_LOGW(TAG, "Authentication token is required before connecting");
        return false;
    }
    if (client_handle_) {
        Disconnect();
    }

    current_ip_ = ip;
    current_port_ = port;
    rx_buffer_.clear();

    char uri_buf[128];
    snprintf(uri_buf, sizeof(uri_buf), "ws://%s:%d", ip.c_str(), port);

    ESP_LOGI(TAG, "Connecting to WebSocket URL: %s", uri_buf);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = uri_buf;
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
    if (connected_.load() || timeout_ms <= 0) return false;

    bool expected = false;
    if (!discovery_running_.compare_exchange_strong(expected, true)) return false;

    auto* args = new (std::nothrow) DiscoveryTaskArgs{
        this,
        timeout_ms,
        std::max(0, initial_delay_ms)
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

void CodexWsClient::DiscoveryTask(void* task_args) {
    auto* args = static_cast<DiscoveryTaskArgs*>(task_args);
    CodexWsClient* client = args->client;
    const int timeout_ms = args->timeout_ms;
    const int initial_delay_ms = args->initial_delay_ms;
    delete args;

    if (initial_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(initial_delay_ms));
    }

    int sock = -1;
    if (!client->connected_.load()) {
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

        while (!client->connected_.load() &&
               xTaskGetTickCount() - started_at < timeout_ticks) {
            sendto(sock, kDiscoveryRequest, strlen(kDiscoveryRequest), 0,
                   reinterpret_cast<sockaddr*>(&destination), sizeof(destination));

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
    } else if (!client->connected_.load()) {
        ESP_LOGW(TAG, "Could not create LAN discovery socket");
    }

    client->discovery_running_ = false;
    vTaskDelete(nullptr);
}

void CodexWsClient::Disconnect() {
    if (client_handle_) {
        esp_websocket_client_stop(client_handle_);
        esp_websocket_client_destroy(client_handle_);
        client_handle_ = nullptr;
    }
    connected_ = false;
    if (on_status_cb_) {
        on_status_cb_(false);
    }
}

bool CodexWsClient::SendTextMessage(const std::string& json_str) {
    if (!client_handle_ || !connected_.load()) {
        ESP_LOGE(TAG, "Cannot send text: Client not connected");
        return false;
    }

    int res = esp_websocket_client_send_text(client_handle_, json_str.c_str(), json_str.length(), portMAX_DELAY);
    return res >= 0;
}

bool CodexWsClient::SendOpusAudioFrame(const uint8_t* data, size_t length) {
    if (!client_handle_ || !connected_.load()) {
        ESP_LOGE(TAG, "Cannot send audio: Client not connected");
        return false;
    }

    int res = esp_websocket_client_send_bin(client_handle_, (const char*)data, length, portMAX_DELAY);
    return res >= 0;
}

void CodexWsClient::EventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    CodexWsClient* client = static_cast<CodexWsClient*>(handler_args);
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            client->connected_ = true;
            if (client->on_status_cb_) {
                client->on_status_cb_(true);
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
            client->connected_ = false;
            client->rx_buffer_.clear();
            if (client->on_status_cb_) {
                client->on_status_cb_(false);
            }
            if (client->HasToken()) client->StartDiscovery(5000, 1500);
            break;

        case WEBSOCKET_EVENT_DATA:
            // A text frame can be delivered in several events by ESP-IDF.
            if (data->op_code == 0x01 && data->data_ptr && data->data_len > 0) {
                if (data->payload_offset == 0) client->rx_buffer_.clear();
                client->rx_buffer_.append(data->data_ptr, data->data_len);

                const int payload_length = data->payload_len > 0
                    ? data->payload_len
                    : static_cast<int>(client->rx_buffer_.size());
                if (data->payload_offset + data->data_len >= payload_length) {
                    ESP_LOGI(TAG, "Received text message (%u bytes)",
                             static_cast<unsigned>(client->rx_buffer_.size()));
                    if (client->on_message_cb_) {
                        client->on_message_cb_(client->rx_buffer_);
                    }
                    client->rx_buffer_.clear();
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
            break;
    }
}
