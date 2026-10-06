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
#include <utility>
#include <unistd.h>

extern "C" bool codex_remote_handle_notification_root(
    const cJSON* root, uint32_t app_generation,
    uint32_t connection_generation, uint32_t connection_epoch);

static const char* TAG = "CodexWsClient";
static const char* NVS_NAMESPACE = "codex_remote";

namespace {
constexpr int kDiscoveryPort = 8766;
constexpr int kDiscoveryProtocolVersion = 1;
constexpr int kDiscoveryAttemptMs = 750;
constexpr size_t kRxQueueDepth = 8;
constexpr size_t kTxQueueDepth = 8;
constexpr size_t kMaxQueuedRxBytes = 1024 * 1024;
constexpr size_t kMaxQueuedTxBytes = 64 * 1024;
constexpr size_t kMaxTxMessageBytes = 32 * 1024;
// cJSON's ESP32-P4 parse_value frame is 96 bytes on RISC-V. The legal depth-64
// preflight permits roughly 6 KiB of parser frames alone; leave room for outer
// parser frames and the post-parse recursive DOM measurement.
constexpr uint32_t kPipelineTaskStackBytes = 12 * 1024;
// This worker also calls WebSocket start/stop/destroy while holding the
// endpoint lock, so budget SDK lifecycle frames as well as bounded sends.
constexpr uint32_t kTxTaskStackBytes = 8 * 1024;
constexpr TickType_t kQueuedSendTimeout = pdMS_TO_TICKS(250);
constexpr char kDiscoveryRequest[] =
    "{\"type\":\"codex-remote-discovery\",\"protocolVersion\":1}";

template <typename T>
void UpdatePeak(std::atomic<T>& peak, T value) {
    T observed = peak.load(std::memory_order_relaxed);
    while (observed < value && !peak.compare_exchange_weak(
        observed, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

struct DiscoveryTaskArgs {
    CodexWsClient* client;
    int timeout_ms;
    int initial_delay_ms;
    uint32_t epoch;
};
}

struct CodexWsClient::RawMessage {
    std::unique_ptr<uint8_t[]> payload;
    size_t payload_size = 0;
    uint32_t app_generation = 0;
    uint32_t connection_generation = 0;
    uint32_t explicit_epoch = 0;
};

enum class CodexWsClient::TxKind : uint8_t { Text, Binary };

struct CodexWsClient::TxMessage {
    std::string payload;
    SendCompletion completion;
    TxKind kind = TxKind::Text;
    uint32_t app_generation = 0;
    uint32_t connection_generation = 0;
    uint32_t connection_epoch = 0;
    size_t charged_bytes = 0;
};

CodexWsMessage::~CodexWsMessage() {
    if (root != nullptr) cJSON_Delete(root);
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
    DisconnectWorker();
}

esp_err_t CodexWsClient::Init() {
    ESP_LOGI(TAG, "Initializing Codex WebSocket Client");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err == ESP_OK && EnsurePipeline() ? ESP_OK : err == ESP_OK ? ESP_ERR_NO_MEM : err;
}

bool CodexWsClient::EnsurePipeline() {
    std::lock_guard<std::mutex> lock(pipeline_setup_mutex_);
    if (pipeline_started_) return true;

    if (rx_queue_ == nullptr) rx_queue_ = xQueueCreate(kRxQueueDepth, sizeof(RawMessage*));
    if (tx_queue_ == nullptr) tx_queue_ = xQueueCreate(kTxQueueDepth, sizeof(TxMessage*));
    if (reclaim_queue_ == nullptr) {
        reclaim_queue_ = xQueueCreate(codex_remote::pipeline::kMaxRetainedMessages,
                                      sizeof(CodexWsMessage*));
    }
    if (rx_queue_ == nullptr || tx_queue_ == nullptr || reclaim_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate bounded WebSocket pipeline queues");
        return false;
    }
    if (pipeline_queue_set_ == nullptr) {
        const UBaseType_t queue_set_items = static_cast<UBaseType_t>(
            kRxQueueDepth + codex_remote::pipeline::kMaxRetainedMessages);
        QueueSetHandle_t set = xQueueCreateSet(queue_set_items);
        if (set == nullptr || xQueueAddToSet(rx_queue_, set) != pdPASS ||
            xQueueAddToSet(reclaim_queue_, set) != pdPASS) {
            if (set != nullptr) vQueueDelete(set);
            ESP_LOGE(TAG, "Failed to create bounded WebSocket receive queue set");
            return false;
        }
        pipeline_queue_set_ = set;
    }
    if (tx_task_ == nullptr && xTaskCreate(TxTask, "codex_ws_tx", kTxTaskStackBytes,
                                           this, 5, &tx_task_) != pdPASS) {
        tx_task_ = nullptr;
        ESP_LOGE(TAG, "Failed to create bounded WebSocket send task");
        return false;
    }
    if (pipeline_task_ == nullptr && xTaskCreate(PipelineTask, "codex_ws_rx",
                                                 kPipelineTaskStackBytes, this, 5,
                                                 &pipeline_task_) != pdPASS) {
        pipeline_task_ = nullptr;
        ESP_LOGE(TAG, "Failed to create bounded WebSocket receive task");
        return false;
    }
    pipeline_started_ = true;
    return true;
}

void CodexWsClient::PipelineTask(void* task_args) {
    auto* client = static_cast<CodexWsClient*>(task_args);
    for (;;) {
        QueueSetMemberHandle_t selected = nullptr;
        if (client->deferred_rx_events_ != 0) {
            --client->deferred_rx_events_;
            selected = client->rx_queue_;
        } else {
            selected = xQueueSelectFromSet(client->pipeline_queue_set_, pdMS_TO_TICKS(100));
        }
        if (selected == client->reclaim_queue_) {
            CodexWsMessage* reclaim = nullptr;
            if (xQueueReceive(client->reclaim_queue_, &reclaim, 0) == pdPASS) delete reclaim;
        } else if (selected == client->rx_queue_) {
            RawMessage* raw = nullptr;
            if (xQueueReceive(client->rx_queue_, &raw, 0) == pdPASS) {
                if (raw != nullptr) {
                    client->rx_queued_bytes_.fetch_sub(raw->payload_size, std::memory_order_acq_rel);
                    client->ProcessReceivedFrame(raw);
                    delete raw;
                }
            }
        }

        if (client->rx_reconnect_requested_.exchange(false, std::memory_order_acq_rel)) {
            client->RequestReconnectFromPipeline();
        }
        client->MaybeLogPipelineStats();
    }
}

void CodexWsClient::TxTask(void* task_args) {
    auto* client = static_cast<CodexWsClient*>(task_args);
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        bool disconnect_requested = false;
        bool connect_requested = false;
        bool reconnect_requested = false;
        std::string connect_address;
        PendingReconnectRequest reconnect_request;
        {
            // The flags and their associated payloads form one request snapshot.
            // A producer arriving after this point leaves a complete request
            // for the next worker pass rather than pairing a flag with the
            // wrong endpoint or reconnect claim.
            std::lock_guard<std::mutex> lock(client->endpoint_request_mutex_);
            disconnect_requested =
                client->endpoint_disconnect_requested_.exchange(false, std::memory_order_acq_rel);
            connect_requested =
                client->endpoint_connect_requested_.exchange(false, std::memory_order_acq_rel);
            if (connect_requested) connect_address.swap(client->pending_connect_address_);
            reconnect_requested =
                client->endpoint_reconnect_requested_.exchange(false, std::memory_order_acq_rel);
            if (reconnect_requested) {
                reconnect_request = client->pending_reconnect_request_;
                client->pending_reconnect_request_ = {};
            }
        }
        if (disconnect_requested) {
            client->DisconnectWorker();
        }
        if (connect_requested) {
            if (client->app_active_.load(std::memory_order_acquire) && !connect_address.empty() &&
                !client->Connect(connect_address)) {
                ESP_LOGW(TAG, "Asynchronous WebSocket connect request failed");
                client->NotifyDisconnected();
            }
        }
        if (reconnect_requested) {
            if (client->app_active_.load(std::memory_order_acquire)) {
                if (!reconnect_request.context_bound) {
                    (void)client->Reconnect();
                } else if (reconnect_request.claim_valid &&
                           client->app_session_generation_.load(std::memory_order_acquire) ==
                               reconnect_request.app_generation &&
                           client->transport_state_.Generation() == reconnect_request.connection_generation) {
                    uint32_t expected_epoch = reconnect_request.claimed_epoch;
                    // This CAS is the execution claim: an explicit connect or
                    // disconnect queued first owns the next transport instead.
                    if (client->explicit_connection_epoch_.compare_exchange_strong(
                            expected_epoch, reconnect_request.claimed_epoch + 1,
                            std::memory_order_acq_rel, std::memory_order_acquire)) {
                        (void)client->Reconnect();
                    }
                }
            }
        }

        TxMessage* tx = nullptr;
        while (xQueueReceive(client->tx_queue_, &tx, 0) == pdPASS) {
            if (tx != nullptr) client->ProcessQueuedSend(tx);
        }
    }
}

void CodexWsClient::SetOnMessageCallback(MessageCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_message_cb_ = std::move(cb);
}

void CodexWsClient::SetOnStatusCallback(StatusCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_status_cb_ = std::move(cb);
}

void CodexWsClient::SetOnDiscoveryCallback(DiscoveryCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_discovery_cb_ = std::move(cb);
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
    if (!EnsurePipeline()) return false;
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
        DisconnectWorker();
    }

    current_uri_ = uri;
    {
        std::lock_guard<std::mutex> info_lock(endpoint_info_mutex_);
        current_ip_ = host;
        current_port_ = port;
    }
    ClearRxAssembly();
    explicit_connection_epoch_.fetch_add(1, std::memory_order_acq_rel);

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
    callback_client_handle_.store(client_handle_, std::memory_order_release);

    if (!app_active_.load(std::memory_order_acquire)) {
        DisconnectWorker();
        return false;
    }

    esp_websocket_register_events(client_handle_, WEBSOCKET_EVENT_ANY, EventHandler, this);

    esp_err_t err = esp_websocket_client_start(client_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        callback_client_handle_.store(nullptr, std::memory_order_release);
        esp_websocket_client_destroy(client_handle_);
        client_handle_ = nullptr;
        NotifyDisconnected();
        return false;
    }

    return true;
}

bool CodexWsClient::RequestConnectAsync(const std::string& address) {
    if (address.empty() || address.size() > 256 ||
        !app_active_.load(std::memory_order_acquire) || !EnsurePipeline()) return false;
    {
        std::lock_guard<std::mutex> lock(endpoint_request_mutex_);
        if (!app_active_.load(std::memory_order_acquire)) return false;
        pending_connect_address_ = address;
        endpoint_connect_requested_.store(true, std::memory_order_release);
        explicit_connection_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    ++discovery_epoch_;
    if (tx_task_ != nullptr) xTaskNotifyGive(tx_task_);
    return true;
}

bool CodexWsClient::HasToken() const {
    std::string token;
    return LoadToken(token);
}

bool CodexWsClient::StartDiscovery(int timeout_ms, int initial_delay_ms) {
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
    if (app_active_.exchange(active, std::memory_order_acq_rel) == active) return;
    app_session_generation_.fetch_add(1, std::memory_order_acq_rel);
    ++discovery_epoch_;
    {
        std::lock_guard<std::mutex> lock(endpoint_request_mutex_);
        explicit_connection_epoch_.fetch_add(1, std::memory_order_acq_rel);
        if (!active) {
            endpoint_disconnect_requested_.store(true, std::memory_order_release);
            endpoint_connect_requested_.store(false, std::memory_order_release);
            endpoint_reconnect_requested_.store(false, std::memory_order_release);
            pending_connect_address_.clear();
            pending_reconnect_request_ = {};
        }
    }
    if (tx_task_ != nullptr) xTaskNotifyGive(tx_task_);
}

void CodexWsClient::SetDiscoveryEnabled(bool enabled) {
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
            {
                std::lock_guard<std::mutex> info_lock(client->endpoint_info_mutex_);
                client->current_ip_ = discovered_ip;
                client->current_port_ = discovered_port;
            }
            DiscoveryCallback callback;
            {
                std::lock_guard<std::mutex> callback_lock(client->callback_mutex_);
                callback = client->on_discovery_cb_;
            }
            if (callback) callback(discovered_name, discovered_ip, discovered_port);
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
    {
        std::lock_guard<std::mutex> lock(endpoint_request_mutex_);
        explicit_connection_epoch_.fetch_add(1, std::memory_order_acq_rel);
        endpoint_disconnect_requested_.store(true, std::memory_order_release);
        endpoint_connect_requested_.store(false, std::memory_order_release);
        endpoint_reconnect_requested_.store(false, std::memory_order_release);
        pending_connect_address_.clear();
        pending_reconnect_request_ = {};
    }
    if (tx_task_ != nullptr) xTaskNotifyGive(tx_task_);
}

void CodexWsClient::DisconnectWorker() {
    std::lock_guard<std::recursive_mutex> lock(endpoint_mutex_);
    // Clear the active handle before stop/destroy: an event from this explicit
    // shutdown is stale by definition and must not alter a later connection.
    esp_websocket_client_handle_t handle = client_handle_;
    client_handle_ = nullptr;
    callback_client_handle_.store(nullptr, std::memory_order_release);
    explicit_connection_epoch_.fetch_add(1, std::memory_order_acq_rel);
    NotifyDisconnected();
    ClearRxAssembly();
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

bool CodexWsClient::RequestReconnectAsync() {
    if (!app_active_.load(std::memory_order_acquire) || !EnsurePipeline()) return false;
    {
        std::lock_guard<std::mutex> lock(endpoint_request_mutex_);
        pending_reconnect_request_ = {};
        endpoint_reconnect_requested_.store(true, std::memory_order_release);
        explicit_connection_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    if (tx_task_ != nullptr) xTaskNotifyGive(tx_task_);
    return true;
}

bool CodexWsClient::RequestReconnectAsync(const SendContext& context) {
    if (!context.valid || !EnsurePipeline()) return false;
    const uint32_t claimed_epoch = context.connection_epoch + 1;
    {
        std::lock_guard<std::mutex> lock(endpoint_request_mutex_);
        if (!app_active_.load(std::memory_order_acquire) ||
            app_session_generation_.load(std::memory_order_acquire) != context.app_generation ||
            transport_state_.Generation() != context.connection_generation ||
            explicit_connection_epoch_.load(std::memory_order_acquire) != context.connection_epoch) {
            return false;
        }
        // Publish a context-bound (initially unclaimed) entry before notifying
        // the worker, so it can never mistake a failed claim for a legacy
        // unconditional reconnect.
        pending_reconnect_request_ = {};
        pending_reconnect_request_.context_bound = true;
        pending_reconnect_request_.app_generation = context.app_generation;
        pending_reconnect_request_.connection_generation = context.connection_generation;
        // Prevent a concurrent CaptureSendContext from adopting the new epoch
        // before the matching reconnect request has been published.
        endpoint_reconnect_requested_.store(true, std::memory_order_release);
        uint32_t expected_epoch = context.connection_epoch;
        if (!explicit_connection_epoch_.compare_exchange_strong(
                expected_epoch, claimed_epoch,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            pending_reconnect_request_.claim_valid = false;
            endpoint_reconnect_requested_.store(false, std::memory_order_release);
            return false;
        }
        if (!app_active_.load(std::memory_order_acquire) ||
            app_session_generation_.load(std::memory_order_acquire) != context.app_generation ||
            transport_state_.Generation() != context.connection_generation ||
            explicit_connection_epoch_.load(std::memory_order_acquire) != claimed_epoch) {
            pending_reconnect_request_.claim_valid = false;
            endpoint_reconnect_requested_.store(false, std::memory_order_release);
            return false;
        }
        pending_reconnect_request_.claimed_epoch = claimed_epoch;
        pending_reconnect_request_.claim_valid = true;
        endpoint_reconnect_requested_.store(true, std::memory_order_release);
    }
    if (tx_task_ != nullptr) xTaskNotifyGive(tx_task_);
    return true;
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

CodexWsClient::SendAdmission CodexWsClient::QueueTextMessage(
    std::string json_str, SendCompletion completion) {
    const SendContext context = CaptureSendContext();
    return QueueTextMessage(std::move(json_str), context, std::move(completion));
}

CodexWsClient::SendAdmission CodexWsClient::QueueTextMessage(
    std::string json_str, const SendContext& context, SendCompletion completion) {
    return QueuePayload(std::move(json_str), TxKind::Text, context, std::move(completion));
}

CodexWsClient::SendContext CodexWsClient::CaptureSendContext() const {
    SendContext context;
    if (endpoint_disconnect_requested_.load(std::memory_order_acquire) ||
        endpoint_reconnect_requested_.load(std::memory_order_acquire) ||
        endpoint_connect_requested_.load(std::memory_order_acquire)) return context;
    for (int attempt = 0; attempt < 3; ++attempt) {
        context.app_generation = app_session_generation_.load(std::memory_order_acquire);
        context.connection_generation = transport_state_.Generation();
        context.connection_epoch = explicit_connection_epoch_.load(std::memory_order_acquire);
        if (!app_active_.load(std::memory_order_acquire) || !transport_state_.IsConnected()) {
            return {};
        }
        if (context.app_generation == app_session_generation_.load(std::memory_order_acquire) &&
            context.connection_generation == transport_state_.Generation() &&
            context.connection_epoch == explicit_connection_epoch_.load(std::memory_order_acquire)) {
            context.valid = true;
            return context;
        }
    }
    return {};
}

CodexWsClient::SendAdmission CodexWsClient::QueuePayload(
    std::string payload, TxKind kind, const SendContext& context,
    SendCompletion completion) {
    const auto reject = [this](SendAdmission reason) {
        tx_rejected_events_.fetch_add(1, std::memory_order_relaxed);
        return reason;
    };
    if (payload.empty()) return reject(SendAdmission::TooLarge);
    if (payload.size() > kMaxTxMessageBytes) return reject(SendAdmission::TooLarge);
    if (!context.valid) {
        if (!app_active_.load(std::memory_order_acquire)) return reject(SendAdmission::Inactive);
        if (!transport_state_.IsConnected()) return reject(SendAdmission::Disconnected);
        return reject(SendAdmission::Stale);
    }
    if (context.app_generation != app_session_generation_.load(std::memory_order_acquire) ||
        context.connection_generation != transport_state_.Generation() ||
        context.connection_epoch != explicit_connection_epoch_.load(std::memory_order_acquire)) {
        return reject(SendAdmission::Stale);
    }
    if (!app_active_.load(std::memory_order_acquire)) return reject(SendAdmission::Inactive);
    if (!transport_state_.IsConnected()) return reject(SendAdmission::Disconnected);
    if (!EnsurePipeline()) return reject(SendAdmission::Unavailable);

    const size_t charge = payload.size();
    size_t queued = tx_queued_bytes_.load(std::memory_order_relaxed);
    do {
        if (charge > kMaxQueuedTxBytes - std::min(queued, kMaxQueuedTxBytes))
            return reject(SendAdmission::Full);
    } while (!tx_queued_bytes_.compare_exchange_weak(
        queued, queued + charge, std::memory_order_acq_rel, std::memory_order_relaxed));
    auto* message = new (std::nothrow) TxMessage{
        .payload = std::move(payload),
        .completion = std::move(completion),
        .kind = kind,
        .app_generation = context.app_generation,
        .connection_generation = context.connection_generation,
        .connection_epoch = context.connection_epoch,
        .charged_bytes = charge,
    };
    if (message == nullptr) {
        tx_queued_bytes_.fetch_sub(charge, std::memory_order_acq_rel);
        return reject(SendAdmission::Unavailable);
    }
    if (xQueueSend(tx_queue_, &message, 0) != pdPASS) {
        tx_queued_bytes_.fetch_sub(charge, std::memory_order_acq_rel);
        delete message;
        return reject(SendAdmission::Full);
    }
    UpdatePeak(tx_queued_bytes_peak_, tx_queued_bytes_.load(std::memory_order_relaxed));
    UpdatePeak(tx_queued_items_peak_, static_cast<size_t>(uxQueueMessagesWaiting(tx_queue_)));
    if (tx_task_ != nullptr) xTaskNotifyGive(tx_task_);
    return SendAdmission::Accepted;
}

CodexWsClient::SendAdmission CodexWsClient::QueueOpusAudioFrame(
    const uint8_t* data, size_t length, SendCompletion completion) {
    return QueueOpusAudioFrame(data, length, CaptureSendContext(), std::move(completion));
}

CodexWsClient::SendAdmission CodexWsClient::QueueOpusAudioFrame(
    const uint8_t* data, size_t length, const SendContext& context,
    SendCompletion completion) {
    constexpr size_t kMaxRawOpusBytes = 4096;
    if (data == nullptr || length == 0 || length > kMaxRawOpusBytes) {
        return SendAdmission::TooLarge;
    }
    std::string payload(reinterpret_cast<const char*>(data), length);
    return QueuePayload(std::move(payload), TxKind::Binary, context, std::move(completion));
}

CodexWsClient::SendAdmission CodexWsClient::QueueRealtimeOpusAudioFrame(
    const std::string& request_id, uint32_t sequence, const uint8_t* data,
    size_t length, SendCompletion completion) {
    return QueueRealtimeOpusAudioFrame(request_id, sequence, data, length,
                                       CaptureSendContext(), std::move(completion));
}

CodexWsClient::SendAdmission CodexWsClient::QueueRealtimeOpusAudioFrame(
    const std::string& request_id, uint32_t sequence, const uint8_t* data,
    size_t length, const SendContext& context, SendCompletion completion) {
    constexpr size_t kMaxRawOpusBytes = 4096;
    constexpr size_t kMaxEncodedBytes = 5464;
    constexpr size_t kMaxRealtimeJsonBytes = 8192;
    if (request_id.empty() || data == nullptr || length == 0 || length > kMaxRawOpusBytes) {
        return SendAdmission::TooLarge;
    }
    size_t encoded_length = 0;
    const int sizing = mbedtls_base64_encode(nullptr, 0, &encoded_length, data, length);
    if (sizing != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || encoded_length > kMaxEncodedBytes) {
        return SendAdmission::TooLarge;
    }
    std::string encoded(encoded_length, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(),
                              &written, data, length) != 0) {
        return SendAdmission::Unavailable;
    }
    encoded.resize(written);
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr ||
        cJSON_AddStringToObject(root, "type", "realtime_audio_input") == nullptr ||
        cJSON_AddStringToObject(root, "requestId", request_id.c_str()) == nullptr ||
        cJSON_AddNumberToObject(root, "sequence", sequence) == nullptr ||
        cJSON_AddNumberToObject(root, "generation", 1) == nullptr ||
        cJSON_AddNumberToObject(root, "sampleRate", 16000) == nullptr ||
        cJSON_AddNumberToObject(root, "frameDuration", 60) == nullptr ||
        cJSON_AddStringToObject(root, "codec", "opus") == nullptr ||
        cJSON_AddStringToObject(root, "data", encoded.c_str()) == nullptr) {
        if (root != nullptr) cJSON_Delete(root);
        return SendAdmission::Unavailable;
    }
    char* printed = cJSON_PrintUnformatted(root);
    const std::string payload = printed != nullptr ? printed : "";
    if (printed != nullptr) cJSON_free(printed);
    cJSON_Delete(root);
    if (payload.empty() || payload.size() > kMaxRealtimeJsonBytes) return SendAdmission::TooLarge;
    return QueuePayload(payload, TxKind::Text, context, std::move(completion));
}

void CodexWsClient::ProcessQueuedSend(TxMessage* tx) {
    if (tx == nullptr) return;
    bool sent = false;
    const auto matches = [this, tx]() {
        return app_active_.load(std::memory_order_acquire) &&
            tx->app_generation == app_session_generation_.load(std::memory_order_acquire) &&
            tx->connection_generation == transport_state_.Generation() &&
            tx->connection_epoch == explicit_connection_epoch_.load(std::memory_order_acquire);
    };
    if (matches()) {
        std::lock_guard<std::recursive_mutex> lock(endpoint_mutex_);
        if (client_handle_ != nullptr && transport_state_.IsConnected() && matches()) {
            const int result = tx->kind == TxKind::Text
                ? esp_websocket_client_send_text(client_handle_, tx->payload.c_str(),
                                                 tx->payload.size(), kQueuedSendTimeout)
                : esp_websocket_client_send_bin(client_handle_, tx->payload.data(),
                                                tx->payload.size(), kQueuedSendTimeout);
            sent = tx->kind == TxKind::Text
                ? result == static_cast<int>(tx->payload.size())
                : result >= 0;
        }
    }
    tx_queued_bytes_.fetch_sub(tx->charged_bytes, std::memory_order_acq_rel);
    if (tx->completion) {
        tx->completion(SendResult{
            .sent = sent,
            .app_generation = tx->app_generation,
            .connection_generation = tx->connection_generation,
            .connection_epoch = tx->connection_epoch,
        });
    }
    delete tx;
}

void CodexWsClient::ReclaimMessage(CodexWsMessage* message) {
    if (message == nullptr) return;
    // ParseBoundedJson reserves one live-message slot before allocating each
    // DOM. The reclaim FIFO has the same number of slots, so every last-owner
    // release can be returned without freeing cJSON nodes on the LVGL task.
    const BaseType_t queued = reclaim_queue_ == nullptr
        ? errQUEUE_FULL
        : xQueueSend(reclaim_queue_, &message, 0);
    if (queued != pdPASS) reclaim_enqueue_errors_.fetch_add(1, std::memory_order_relaxed);
    configASSERT(queued == pdPASS);
    (void)queued;
}

void CodexWsClient::MaybeLogPipelineStats() {
    static TickType_t last_log_tick = 0;
    const TickType_t now = xTaskGetTickCount();
    if (now - last_log_tick < pdMS_TO_TICKS(60000)) return;
    last_log_tick = now;
    const auto dom = retained_dom_budget_.GetStats();
    const UBaseType_t rx_stack_bytes = pipeline_task_ == nullptr
        ? 0 : uxTaskGetStackHighWaterMark(pipeline_task_);
    const UBaseType_t tx_stack_bytes = tx_task_ == nullptr
        ? 0 : uxTaskGetStackHighWaterMark(tx_task_);
    ESP_LOGI(TAG,
        "Pipeline peak rx=%u/%uB tx=%u/%uB assembly=%u/%uB in_flight=%u/%uB "
        "dom=%uB/%u nodes/%u msg reclaim_err=%u rx_overflow=%u tx_reject=%u "
        "stack_hwm_rx=%uB tx=%uB",
        static_cast<unsigned>(rx_queued_items_peak_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(rx_queued_bytes_peak_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(tx_queued_items_peak_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(tx_queued_bytes_peak_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(rx_assembly_bytes_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(rx_assembly_bytes_peak_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(rx_inflight_bytes_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(rx_inflight_bytes_peak_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(dom.high_water_bytes),
        static_cast<unsigned>(dom.high_water_nodes),
        static_cast<unsigned>(dom.high_water_messages),
        static_cast<unsigned>(reclaim_enqueue_errors_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(rx_overflow_events_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(tx_rejected_events_.load(std::memory_order_relaxed)),
        static_cast<unsigned>(rx_stack_bytes), static_cast<unsigned>(tx_stack_bytes));
}

void CodexWsClient::ClearRxAssembly() {
    std::lock_guard<std::mutex> lock(rx_assembly_mutex_);
    rx_buffer_.reset();
    rx_assembly_bytes_.store(0, std::memory_order_relaxed);
    rx_expected_size_ = 0;
    rx_buffer_size_ = 0;
    rx_app_generation_ = 0;
    rx_connection_generation_ = 0;
    rx_explicit_epoch_ = 0;
}

void CodexWsClient::SignalRxOverflow() {
    rx_overflow_events_.fetch_add(1, std::memory_order_relaxed);
    rx_reconnect_requested_.store(true, std::memory_order_release);
    if (rx_queue_ != nullptr) {
        RawMessage* wake = nullptr;
        (void)xQueueSend(rx_queue_, &wake, 0);
    }
}

void CodexWsClient::RequestReconnectFromPipeline() {
    if (!app_active_.load(std::memory_order_acquire)) return;
    ESP_LOGW(TAG, "Applying deferred WebSocket reconnect request");
    if (!RequestReconnectAsync()) NotifyDisconnected();
}

void CodexWsClient::ProcessReceivedFrame(RawMessage* raw) {
    if (raw == nullptr || raw->payload == nullptr || raw->payload_size == 0 ||
        !app_active_.load(std::memory_order_acquire) ||
        raw->app_generation != app_session_generation_.load(std::memory_order_acquire) ||
        raw->explicit_epoch != explicit_connection_epoch_.load(std::memory_order_acquire) ||
        raw->connection_generation != transport_state_.Generation() ||
        !transport_state_.IsConnected()) return;
    rx_inflight_bytes_.store(raw->payload_size, std::memory_order_relaxed);
    UpdatePeak(rx_inflight_bytes_peak_, raw->payload_size);
    struct InflightReset {
        std::atomic<size_t>& bytes;
        ~InflightReset() { bytes.store(0, std::memory_order_relaxed); }
    } inflight_reset{rx_inflight_bytes_};
    const std::string_view frame(reinterpret_cast<const char*>(raw->payload.get()),
                                 raw->payload_size);

    // Keep one already-dequeued frame at the head of the ordered stream while
    // its DOM slot is occupied. This worker services reclaim and records
    // receive-queue set signals for later FIFO dequeue; TX has its own worker.
    while (retained_dom_budget_.GetStats().messages >=
               codex_remote::pipeline::kMaxRetainedMessages &&
           app_active_.load(std::memory_order_acquire) &&
           raw->app_generation == app_session_generation_.load(std::memory_order_acquire) &&
           raw->connection_generation == transport_state_.Generation() &&
           raw->explicit_epoch == explicit_connection_epoch_.load(std::memory_order_acquire)) {
        const QueueSetMemberHandle_t selected = xQueueSelectFromSet(
            pipeline_queue_set_, pdMS_TO_TICKS(100));
        if (selected == reclaim_queue_) {
            CodexWsMessage* released = nullptr;
            if (xQueueReceive(reclaim_queue_, &released, 0) == pdPASS) delete released;
        } else if (selected == rx_queue_) {
            ++deferred_rx_events_;
        }
        if (rx_reconnect_requested_.exchange(false, std::memory_order_acq_rel)) {
            RequestReconnectFromPipeline();
            return;
        }
    }
    if (!app_active_.load(std::memory_order_acquire) ||
        raw->app_generation != app_session_generation_.load(std::memory_order_acquire) ||
        raw->connection_generation != transport_state_.Generation() ||
        raw->explicit_epoch != explicit_connection_epoch_.load(std::memory_order_acquire) ||
        !transport_state_.IsConnected()) return;

    codex_remote::pipeline::ParsedJson parsed;
    auto parsed_result = codex_remote::pipeline::ParseBoundedJson(
        frame, &retained_dom_budget_, &parsed);
    bool state_omitted = false;
    std::string compact_ack;
    std::string_view type;
    const bool have_type = codex_remote::pipeline::ReadTopLevelStringMember(
        frame, "type", &type);
    if (parsed_result != codex_remote::pipeline::CheckResult::Ok) {
        const bool recoverable_size = parsed_result == codex_remote::pipeline::CheckResult::TooLarge ||
            parsed_result == codex_remote::pipeline::CheckResult::TooManyNodes ||
            parsed_result == codex_remote::pipeline::CheckResult::DomLimit ||
            parsed_result == codex_remote::pipeline::CheckResult::InternalLimit;
        if (recoverable_size && have_type && type == "codex_action_result" &&
            codex_remote::pipeline::ElideTopLevelMember(frame, "state", &compact_ack)) {
            parsed_result = codex_remote::pipeline::ParseBoundedJson(
                compact_ack, &retained_dom_budget_, &parsed);
            state_omitted = parsed_result == codex_remote::pipeline::CheckResult::Ok;
        }
        if (parsed_result != codex_remote::pipeline::CheckResult::Ok) {
            static std::atomic<uint32_t> rejected_frames{0};
            if ((rejected_frames.fetch_add(1, std::memory_order_relaxed) & 63u) == 0) {
                ESP_LOGW(TAG, "Rejected bounded WebSocket frame (reason=%u, bytes=%u)",
                         static_cast<unsigned>(parsed_result),
                         static_cast<unsigned>(raw->payload_size));
            }
            if (have_type && type == "codex_state" && recoverable_size) {
                auto notice = std::make_shared<CodexWsMessage>();
                notice->app_generation = raw->app_generation;
                notice->connection_generation = raw->connection_generation;
                notice->connection_epoch = raw->explicit_epoch;
                notice->notice = CodexWsMessage::Notice::StateTooLarge;
                MessageCallback callback;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    callback = on_message_cb_;
                }
                while (callback && app_active_.load(std::memory_order_acquire) &&
                       raw->app_generation == app_session_generation_.load(std::memory_order_acquire) &&
                       raw->connection_generation == transport_state_.Generation() &&
                       raw->explicit_epoch == explicit_connection_epoch_.load(std::memory_order_acquire)) {
                    if (callback(notice)) break;
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            return;
        }
    }

    if (!app_active_.load(std::memory_order_acquire) ||
        raw->app_generation != app_session_generation_.load(std::memory_order_acquire) ||
        raw->connection_generation != transport_state_.Generation() ||
        raw->explicit_epoch != explicit_connection_epoch_.load(std::memory_order_acquire) ||
        !transport_state_.IsConnected()) return;

    cJSON* root = parsed.root;
    const uint32_t app_generation = raw->app_generation;
    if (root == nullptr) return;
    const bool notification_accepted = codex_remote_handle_notification_root(
        root, app_generation, raw->connection_generation, raw->explicit_epoch);
    (void)notification_accepted;

    std::vector<uint8_t> audio;
    bool audio_valid = false;
    const cJSON* type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type_item) &&
        std::strcmp(type_item->valuestring, "realtime_audio") == 0) {
        const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsString(data) && data->valuestring != nullptr) {
            audio_valid = codex_remote::pipeline::DecodeBase64(
                data->valuestring,
                codex_remote::pipeline::kMaxRealtimeAudioBytes, &audio);
        }
        cJSON_DeleteItemFromObjectCaseSensitive(root, "data");
    }

    codex_remote::pipeline::Footprint footprint{};
    parsed_result = codex_remote::pipeline::MeasureDom(root, &footprint);
    if (parsed_result != codex_remote::pipeline::CheckResult::Ok) return;
    codex_remote::pipeline::Footprint accounted = footprint;
    if (audio_valid) {
        if (audio.size() > codex_remote::pipeline::kMaxFrameDomBytes - accounted.dom_bytes) return;
        accounted.dom_bytes += audio.size();
    } else {
        audio.clear();
    }
    if (!parsed.reservation.ShrinkTo(accounted)) return;

    auto* message = new (std::nothrow) CodexWsMessage();
    if (message == nullptr) return;
    message->root = std::exchange(parsed.root, nullptr);
    message->app_generation = raw->app_generation;
    message->connection_generation = raw->connection_generation;
    message->connection_epoch = raw->explicit_epoch;
    message->realtime_audio = std::move(audio);
    message->realtime_audio_valid = audio_valid;
    message->state_omitted = state_omitted;
    message->retained_bytes = accounted.dom_bytes;
    message->reservation = std::move(parsed.reservation);

    std::shared_ptr<const CodexWsMessage> shared(message, [this](const CodexWsMessage* released) {
        ReclaimMessage(const_cast<CodexWsMessage*>(released));
    });
    MessageCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = on_message_cb_;
    }
    while (callback && app_active_.load(std::memory_order_acquire) &&
           message->app_generation == app_session_generation_.load(std::memory_order_acquire) &&
           message->connection_generation == transport_state_.Generation() &&
           raw->explicit_epoch == explicit_connection_epoch_.load(std::memory_order_acquire)) {
        if (callback(shared)) return;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
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
    if (!client || !client->app_active_.load(std::memory_order_acquire) || !data ||
        data->client != client->callback_client_handle_.load(std::memory_order_acquire)) {
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
                bool overflow = false;
                RawMessage* completed = nullptr;
                size_t bytes = 0;
                {
                    std::lock_guard<std::mutex> assembly_lock(client->rx_assembly_mutex_);
                    if (data->payload_offset == 0) {
                        const size_t declared_size = data->payload_len > 0
                            ? static_cast<size_t>(data->payload_len)
                            : static_cast<size_t>(data->data_len);
                        if (client->rx_buffer_ != nullptr || declared_size == 0 ||
                            declared_size > codex_remote::pipeline::kMaxWireBytes) {
                            overflow = true;
                        } else {
                            client->rx_buffer_.reset(new (std::nothrow) uint8_t[declared_size]);
                            if (client->rx_buffer_ == nullptr) {
                                overflow = true;
                            } else {
                                client->rx_expected_size_ = declared_size;
                                client->rx_buffer_size_ = 0;
                                client->rx_assembly_bytes_.store(declared_size, std::memory_order_relaxed);
                                UpdatePeak(client->rx_assembly_bytes_peak_, declared_size);
                                client->rx_app_generation_ = client->app_session_generation_.load(std::memory_order_acquire);
                                client->rx_connection_generation_ = client->transport_state_.Generation();
                                client->rx_explicit_epoch_ = client->explicit_connection_epoch_.load(std::memory_order_acquire);
                            }
                        }
                    }
                    const size_t chunk_size = static_cast<size_t>(data->data_len);
                    const size_t offset = data->payload_offset < 0
                        ? codex_remote::pipeline::kMaxWireBytes + 1
                        : static_cast<size_t>(data->payload_offset);
                    if (overflow || client->rx_buffer_ == nullptr || data->payload_offset < 0 ||
                        offset != client->rx_buffer_size_ || offset > client->rx_expected_size_ ||
                        chunk_size > client->rx_expected_size_ - offset ||
                        (data->payload_len > 0 &&
                         static_cast<size_t>(data->payload_len) != client->rx_expected_size_)) {
                        overflow = true;
                    } else {
                        std::memcpy(client->rx_buffer_.get() + offset, data->data_ptr, chunk_size);
                        client->rx_buffer_size_ += chunk_size;
                        if (client->rx_buffer_size_ == client->rx_expected_size_) {
                            RawMessage* message = new (std::nothrow) RawMessage();
                            if (message == nullptr) {
                                overflow = true;
                            } else {
                                bytes = client->rx_buffer_size_;
                                message->payload = std::move(client->rx_buffer_);
                                message->payload_size = bytes;
                                message->app_generation = client->rx_app_generation_;
                                message->connection_generation = client->rx_connection_generation_;
                                message->explicit_epoch = client->rx_explicit_epoch_;
                                client->rx_expected_size_ = 0;
                                client->rx_buffer_size_ = 0;
                                client->rx_assembly_bytes_.store(0, std::memory_order_relaxed);
                                client->rx_app_generation_ = 0;
                                client->rx_connection_generation_ = 0;
                                client->rx_explicit_epoch_ = 0;
                                completed = message;
                            }
                        }
                    }
                }
                if (overflow) {
                    client->SignalRxOverflow();
                    break;
                }
                if (completed != nullptr) {
                    size_t queued = client->rx_queued_bytes_.load(std::memory_order_relaxed);
                    bool reserved = false;
                    do {
                        if (bytes > kMaxQueuedRxBytes - std::min(queued, kMaxQueuedRxBytes)) break;
                        if (client->rx_queued_bytes_.compare_exchange_weak(
                                queued, queued + bytes, std::memory_order_acq_rel,
                                std::memory_order_relaxed)) {
                            reserved = true;
                            break;
                        }
                    } while (true);
                    if (!reserved || xQueueSend(client->rx_queue_, &completed, 0) != pdPASS) {
                        if (reserved) client->rx_queued_bytes_.fetch_sub(bytes, std::memory_order_acq_rel);
                        delete completed;
                        client->SignalRxOverflow();
                    } else {
                        UpdatePeak(client->rx_queued_bytes_peak_,
                                   client->rx_queued_bytes_.load(std::memory_order_relaxed));
                        UpdatePeak(client->rx_queued_items_peak_,
                                   static_cast<size_t>(uxQueueMessagesWaiting(client->rx_queue_)));
                    }
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
    if (transport_state_.MarkConnected()) {
        StatusCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = on_status_cb_;
        }
        if (callback) callback(true, app_session_generation_.load(std::memory_order_acquire),
                                transport_state_.Generation(),
                                explicit_connection_epoch_.load(std::memory_order_acquire));
    }
}

void CodexWsClient::NotifyDisconnected() {
    transport_state_.MarkDisconnected();
    StatusCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = on_status_cb_;
    }
    if (callback) callback(false, app_session_generation_.load(std::memory_order_acquire),
                            transport_state_.Generation(),
                            explicit_connection_epoch_.load(std::memory_order_acquire));
}
