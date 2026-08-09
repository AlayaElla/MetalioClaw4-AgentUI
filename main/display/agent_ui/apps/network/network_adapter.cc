#include "network_adapter.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "application.h"
#include "board.h"
#include "dual_network_board.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "i18n.h"
#include "nt26_board.h"
#include "settings.h"
#include "ssid_manager.h"
#include "wifi_station.h"

namespace agent_ui::network {
namespace {

constexpr char kTag[] = "NetworkAdapter";
constexpr EventBits_t kScanDone = BIT0;
constexpr EventBits_t kConnected = BIT1;
constexpr EventBits_t kDisconnected = BIT2;
constexpr int kNetworkWifi = 0;
constexpr int kNetworkCellular = 1;
constexpr int kSimExternal = 0;
constexpr int kSimInternal = 1;
constexpr std::size_t kMaxSsidLength = 32;
constexpr std::size_t kMaxPasswordLength = 64;

constexpr uint32_t kTextColor = 0xFFFFFF;
constexpr uint32_t kSubtleColor = 0x7A8494;
constexpr uint32_t kSuccessColor = 0x31B77A;
constexpr uint32_t kErrorColor = 0xD14343;
constexpr uint32_t kProgressColor = 0xC48A32;

struct AccessPoint {
    std::string ssid;
    int8_t rssi = -127;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
};

const char* AuthLabel(wifi_auth_mode_t mode) {
    return mode == WIFI_AUTH_OPEN ? I18n::T("[开放]") : I18n::T("[加密]");
}

const char* RssiLabel(int8_t rssi) {
    if (rssi >= -55) return I18n::T("信号强");
    if (rssi >= -65) return I18n::T("信号较强");
    if (rssi >= -75) return I18n::T("信号中");
    if (rssi >= -85) return I18n::T("信号弱");
    return I18n::T("信号很弱");
}

const char* SimSlotName(int slot) {
    return slot == kSimInternal ? I18n::T("内置卡") : I18n::T("外置卡");
}

const char* DisconnectReason(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_LEAVE:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        case WIFI_REASON_GROUP_CIPHER_INVALID:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return I18n::T("密码错误，请重新输入");
        case WIFI_REASON_NO_AP_FOUND:
            return I18n::T("未找到该 WiFi（信号丢失）");
        case WIFI_REASON_ASSOC_TOOMANY:
        case WIFI_REASON_ASSOC_FAIL:
            return I18n::T("关联失败，路由器拒绝连接");
        case WIFI_REASON_BEACON_TIMEOUT:
            return I18n::T("信号太弱，连接超时");
        case 0:
            return I18n::T("连接失败");
        default:
            return nullptr;
    }
}

}  // namespace

struct Adapter::Impl {
    EventSink sink;
    std::atomic<bool> active{false};
    std::atomic<bool> wifi_initialized{false};
    std::atomic<bool> scan_in_progress{false};
    std::atomic<bool> connect_in_progress{false};
    std::atomic<bool> network_switch_pending{false};
    std::atomic<bool> sim_switch_pending{false};

    std::vector<AccessPoint> scan_results;
    std::mutex scan_results_mutex;
    esp_netif_t* netif = nullptr;
    esp_event_handler_instance_t wifi_event_instance = nullptr;
    esp_event_handler_instance_t ip_event_instance = nullptr;
    EventGroupHandle_t events = nullptr;
    bool wifi_station_was_active = false;
    uint8_t last_disconnect_reason = 0;

    void Emit(const Event& event) {
        if (!active.load(std::memory_order_acquire)) return;
        const EventSink callback = sink;
        if (callback) callback(event);
    }

    void EmitStatus(const char* text, uint32_t color = kTextColor) {
        Event event;
        event.type = EventType::Status;
        event.text = text != nullptr ? text : "";
        event.color = color;
        Emit(event);
    }

    bool IsCellularMode() const {
        return DualNetworkBoard::LoadNetworkTypeFromSettings(kNetworkCellular) ==
               NetworkType::ML307;
    }

    void EmitModeSnapshot() {
        Event event;
        event.type = EventType::ModeSnapshot;
        event.cellular = IsCellularMode();
        event.external_slot = GetSavedSimSlot() == kSimExternal;
        Emit(event);
    }

    void EmitSavedNetworks() {
        Event event;
        event.type = EventType::SavedNetworks;
        const auto& list = SsidManager::GetInstance().GetSsidList();
        event.saved_networks.reserve(list.size());
        for (std::size_t i = 0; i < list.size(); ++i) {
            event.saved_networks.push_back({list[i].ssid, i == 0});
        }
        Emit(event);
    }

    void EmitNearbyNetworks(bool scanning, bool scan_started) {
        Event event;
        event.type = EventType::NearbyNetworks;
        event.scanning = scanning;
        event.scan_started = scan_started;
        std::lock_guard<std::mutex> lock(scan_results_mutex);
        event.nearby_networks.reserve(scan_results.size());
        for (const auto& item : scan_results) {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "%s · %s", AuthLabel(item.authmode),
                          RssiLabel(item.rssi));
            event.nearby_networks.push_back({item.ssid, detail});
        }
        Emit(event);
    }

    int GetSavedSimSlot() const {
        Settings settings("network", true);
        const int slot = settings.GetInt("sim_slot", kSimExternal);
        return slot == kSimInternal ? kSimInternal : kSimExternal;
    }

    void SaveSimSlot(int slot) {
        Settings settings("network", true);
        settings.SetInt("sim_slot", slot);
    }

    void SaveNetworkType(int type) {
        Settings settings("network", true);
        settings.SetInt("type", type);
    }

    DualNetworkBoard* GetDualBoard() const {
        return dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    }

    Nt26Board* GetModemBoard() const {
        auto* dual = GetDualBoard();
        if (dual != nullptr) {
            return dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
        }
        return dynamic_cast<Nt26Board*>(&Board::GetInstance());
    }

    static void WifiEvent(void* arg, esp_event_base_t base, int32_t id,
                          void* data) {
        auto* self = static_cast<Impl*>(arg);
        if (self == nullptr || self->events == nullptr) return;
        if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
            xEventGroupSetBits(self->events, kScanDone);
        } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            const auto* event = static_cast<wifi_event_sta_disconnected_t*>(data);
            self->last_disconnect_reason = event != nullptr ? event->reason : 0;
            xEventGroupSetBits(self->events, kDisconnected);
        } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            xEventGroupSetBits(self->events, kConnected);
        }
    }

    bool InitializeWifi() {
        if (wifi_initialized.load(std::memory_order_acquire)) return true;

        wifi_mode_t mode = WIFI_MODE_NULL;
        const esp_err_t mode_error = esp_wifi_get_mode(&mode);
        wifi_station_was_active = mode_error == ESP_OK && mode != WIFI_MODE_NULL;
        if (wifi_station_was_active) WifiStation::GetInstance().Stop();

        if (events == nullptr) {
            events = xEventGroupCreate();
            if (events == nullptr) {
                EmitStatus(I18n::T("无法创建 WiFi 事件状态"), kErrorColor);
                return false;
            }
        } else {
            xEventGroupClearBits(events, kScanDone | kConnected | kDisconnected);
        }

        esp_err_t error = esp_netif_init();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return false;
        error = esp_event_loop_create_default();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return false;

        netif = esp_netif_create_default_wifi_sta();
        if (netif == nullptr) return false;

        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        config.nvs_enable = false;
        error = esp_wifi_init(&config);
        if (error != ESP_OK) {
            netif = nullptr;
            return false;
        }

        error = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEvent, this, &wifi_event_instance);
        if (error != ESP_OK) return false;
        error = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEvent, this, &ip_event_instance);
        if (error != ESP_OK) return false;

        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
            return false;
        }
        wifi_initialized.store(true, std::memory_order_release);
        return true;
    }

    void TeardownWifi() {
        if (!wifi_initialized.exchange(false, std::memory_order_acq_rel)) return;
        esp_wifi_scan_stop();
        esp_wifi_disconnect();
        if (wifi_event_instance != nullptr) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  wifi_event_instance);
            wifi_event_instance = nullptr;
        }
        if (ip_event_instance != nullptr) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  ip_event_instance);
            ip_event_instance = nullptr;
        }
        esp_wifi_stop();
        esp_wifi_deinit();
        if (netif != nullptr) {
            esp_netif_destroy(netif);
            netif = nullptr;
        }
        if (wifi_station_was_active) WifiStation::GetInstance().Start();
        wifi_station_was_active = false;
    }

    static void ScanTaskEntry(void* arg) {
        static_cast<Impl*>(arg)->ScanTask();
    }

    void ScanTask() {
        scan_in_progress.store(true, std::memory_order_release);
        if (!active.load(std::memory_order_acquire)) {
            scan_in_progress.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }
        Event started;
        started.type = EventType::ScanStarted;
        Emit(started);
        EmitNearbyNetworks(true, true);
        EmitStatus(I18n::T("正在扫描附近 WiFi…"), kProgressColor);

        if (!InitializeWifi()) {
            scan_in_progress.store(false, std::memory_order_release);
            EmitStatus(I18n::T("WiFi 初始化失败"), kErrorColor);
            EmitNearbyNetworks(false, true);
            Event finished;
            finished.type = EventType::ScanFinished;
            Emit(finished);
            vTaskDelete(nullptr);
            return;
        }

        if (!active.load(std::memory_order_acquire)) {
            scan_in_progress.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }

        xEventGroupClearBits(events, kScanDone);
        const esp_err_t scan_error = esp_wifi_scan_start(nullptr, false);
        if (scan_error != ESP_OK) {
            scan_in_progress.store(false, std::memory_order_release);
            char status[96];
            std::snprintf(status, sizeof(status), I18n::T("启动扫描失败 (err=%d)"),
                          scan_error);
            EmitStatus(status, kErrorColor);
            EmitNearbyNetworks(false, true);
            Event finished;
            finished.type = EventType::ScanFinished;
            Emit(finished);
            vTaskDelete(nullptr);
            return;
        }

        const EventBits_t bits = xEventGroupWaitBits(
            events, kScanDone, pdTRUE, pdTRUE, pdMS_TO_TICKS(15000));
        if (!(bits & kScanDone)) {
            scan_in_progress.store(false, std::memory_order_release);
            EmitStatus(I18n::T("扫描超时"), kErrorColor);
            EmitNearbyNetworks(false, true);
            Event finished;
            finished.type = EventType::ScanFinished;
            Emit(finished);
            vTaskDelete(nullptr);
            return;
        }

        if (!active.load(std::memory_order_acquire)) {
            scan_in_progress.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }

        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        std::vector<wifi_ap_record_t> records(count);
        if (count > 0) {
            uint16_t received = count;
            esp_wifi_scan_get_ap_records(&received, records.data());
            records.resize(received);
        }

        std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
            return left.rssi > right.rssi;
        });
        std::size_t result_count = 0;
        {
            std::lock_guard<std::mutex> lock(scan_results_mutex);
            scan_results.clear();
            for (const auto& record : records) {
                const char* ssid = reinterpret_cast<const char*>(record.ssid);
                if (ssid == nullptr || ssid[0] == '\0') continue;
                const auto duplicate = std::find_if(
                    scan_results.begin(), scan_results.end(),
                    [ssid](const AccessPoint& item) { return item.ssid == ssid; });
                if (duplicate == scan_results.end()) {
                    scan_results.push_back({ssid, record.rssi, record.authmode});
                }
            }
            result_count = scan_results.size();
        }
        scan_in_progress.store(false, std::memory_order_release);
        EmitNearbyNetworks(false, true);
        char status[96];
        std::snprintf(status, sizeof(status), I18n::T("扫描完成，共 %u 个网络"),
                      static_cast<unsigned>(result_count));
        EmitStatus(status, kSuccessColor);
        Event finished;
        finished.type = EventType::ScanFinished;
        Emit(finished);
        vTaskDelete(nullptr);
    }

    void StartScan() {
        if (!active.load(std::memory_order_acquire)) return;
        if (scan_in_progress.load(std::memory_order_acquire)) return;
        if (connect_in_progress.load(std::memory_order_acquire)) {
            EmitStatus(I18n::T("正在连接，请稍后再扫描"), kProgressColor);
            return;
        }
        if (xTaskCreate(ScanTaskEntry, "wifi_scan", 4096, this, 5, nullptr) != pdPASS) {
            EmitStatus(I18n::T("无法启动扫描任务"), kErrorColor);
        }
    }

    struct ConnectContext {
        Impl* adapter;
        std::string ssid;
        std::string password;
    };

    static void ConnectTaskEntry(void* arg) {
        std::unique_ptr<ConnectContext> context(static_cast<ConnectContext*>(arg));
        context->adapter->ConnectTask(*context);
    }

    void ConnectTask(const ConnectContext& context) {
        if (!active.load(std::memory_order_acquire)) {
            connect_in_progress.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }
        char status[160];
        std::snprintf(status, sizeof(status), I18n::T("正在连接 %s …"),
                      context.ssid.c_str());
        EmitStatus(status, kProgressColor);
        if (!InitializeWifi()) {
            connect_in_progress.store(false, std::memory_order_release);
            EmitFailure(I18n::T("连接失败"), I18n::T("WiFi 初始化失败"));
            vTaskDelete(nullptr);
            return;
        }

        if (!active.load(std::memory_order_acquire)) {
            connect_in_progress.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }

        esp_wifi_scan_stop();
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!active.load(std::memory_order_acquire)) {
            connect_in_progress.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }
        wifi_config_t config = {};
        strlcpy(reinterpret_cast<char*>(config.sta.ssid), context.ssid.c_str(),
                sizeof(config.sta.ssid));
        strlcpy(reinterpret_cast<char*>(config.sta.password), context.password.c_str(),
                sizeof(config.sta.password));
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        config.sta.failure_retry_cnt = 1;
        xEventGroupClearBits(events, kConnected | kDisconnected);
        last_disconnect_reason = 0;

        if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK ||
            esp_wifi_connect() != ESP_OK) {
            connect_in_progress.store(false, std::memory_order_release);
            EmitFailure(I18n::T("连接失败"), I18n::T("无法启动 WiFi 连接"));
            vTaskDelete(nullptr);
            return;
        }

        const EventBits_t bits = xEventGroupWaitBits(
            events, kConnected | kDisconnected, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(15000));
        if (!active.load(std::memory_order_acquire)) {
            connect_in_progress.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }
        if (bits & kConnected) {
            SsidManager::GetInstance().AddSsid(context.ssid, context.password);
            SaveNetworkType(kNetworkWifi);
            EmitSavedNetworks();
            std::snprintf(status, sizeof(status), I18n::T("连接 %s 成功，准备重启…"),
                          context.ssid.c_str());
            EmitStatus(status, kSuccessColor);
            std::snprintf(status, sizeof(status), I18n::T("%s 连接成功！"),
                          context.ssid.c_str());
            BeginRestartCountdown(status, false);
        } else if (bits & kDisconnected) {
            const char* reason = DisconnectReason(last_disconnect_reason);
            if (reason != nullptr) {
                EmitFailure(I18n::T("连接失败"), reason);
            } else {
                std::snprintf(status, sizeof(status), I18n::T("连接被拒绝 (reason=%u)"),
                              static_cast<unsigned>(last_disconnect_reason));
                EmitFailure(I18n::T("连接失败"), status);
            }
        } else {
            esp_wifi_disconnect();
            EmitFailure(I18n::T("连接超时"),
                        I18n::T("未能在 15 秒内完成连接，请检查网络后重试"));
        }
        connect_in_progress.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
    }

    void EmitFailure(const char* title, const char* detail,
                     uint32_t auto_close_ms = 2500) {
        Event event;
        event.type = EventType::ConnectFailed;
        event.text = title != nullptr ? title : I18n::T("连接失败");
        event.detail = detail != nullptr ? detail : "";
        event.auto_close_ms = auto_close_ms;
        event.color = kErrorColor;
        Emit(event);
        EmitStatus(event.detail.c_str(), kErrorColor);
    }

    void StartConnect(const std::string& ssid, const std::string& password) {
        if (!active.load(std::memory_order_acquire)) return;
        if (connect_in_progress.exchange(true, std::memory_order_acq_rel)) {
            EmitStatus(I18n::T("已有正在进行的连接任务"), kProgressColor);
            return;
        }
        if (ssid.empty() || ssid.size() > kMaxSsidLength) {
            connect_in_progress.store(false, std::memory_order_release);
            EmitFailure(I18n::T("SSID 不合法"), I18n::T("SSID 不合法"));
            return;
        }
        if (password.size() > kMaxPasswordLength) {
            connect_in_progress.store(false, std::memory_order_release);
            EmitFailure(I18n::T("密码超长"), I18n::T("密码超长"));
            return;
        }
        Event started;
        started.type = EventType::ConnectStarted;
        started.ssid = ssid;
        Emit(started);
        auto* context = new ConnectContext{this, ssid, password};
        if (xTaskCreate(ConnectTaskEntry, "wifi_connect", 4096, context, 5, nullptr) !=
            pdPASS) {
            delete context;
            connect_in_progress.store(false, std::memory_order_release);
            EmitFailure(I18n::T("连接失败"), I18n::T("无法启动连接任务"));
        }
    }

    void ConnectSaved(std::size_t index) {
        const auto& list = SsidManager::GetInstance().GetSsidList();
        if (index >= list.size()) {
            EmitFailure(I18n::T("连接失败"), I18n::T("已保存的网络不存在"));
            return;
        }
        StartConnect(list[index].ssid, list[index].password);
    }

    struct RestartContext {
        Impl* adapter;
    };

    static void RestartTaskEntry(void* arg) {
        std::unique_ptr<RestartContext> context(static_cast<RestartContext*>(arg));
        Impl* self = context->adapter;
        int remaining = 3;
        while (self->active.load(std::memory_order_acquire) && remaining > 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            --remaining;
            Event event;
            event.type = EventType::RestartCountdown;
            event.value = remaining;
            self->Emit(event);
        }
        if (self->active.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            Application::GetInstance().Reboot();
        }
        vTaskDelete(nullptr);
    }

    void BeginRestartCountdown(const std::string& headline, bool sim_switch) {
        Event event;
        event.type = sim_switch ? EventType::SimSwitchSucceeded
                                : EventType::ConnectSucceeded;
        event.text = headline;
        event.value = 3;
        Emit(event);
        auto* context = new RestartContext{this};
        if (xTaskCreate(RestartTaskEntry, "network_reboot", 2048, context, 5, nullptr) !=
            pdPASS) {
            delete context;
            EmitStatus(I18n::T("无法启动重启任务"), kErrorColor);
        }
    }

    static void NetworkSwitchTaskEntry(void* arg) {
        auto* self = static_cast<Impl*>(arg);
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (self->active.load(std::memory_order_acquire)) {
            if (auto* dual = self->GetDualBoard()) {
                dual->SwitchNetworkType();
            } else {
                esp_restart();
            }
        }
        self->network_switch_pending.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
    }

    void SwitchNetwork() {
        if (!active.load(std::memory_order_acquire)) return;
        if (network_switch_pending.exchange(true, std::memory_order_acq_rel)) return;
        if (GetDualBoard() == nullptr) {
            network_switch_pending.store(false, std::memory_order_release);
            EmitFailure(I18n::T("网络切换失败"), I18n::T("当前设备不支持网络切换"));
            return;
        }
        Event event;
        event.type = EventType::NetworkSwitchStarted;
        event.text = !IsCellularMode() ? I18n::T("准备切换到 4G…")
                                       : I18n::T("准备切换到 WiFi…");
        Emit(event);
        if (xTaskCreate(NetworkSwitchTaskEntry, "network_switch", 4096, this, 5, nullptr) !=
            pdPASS) {
            network_switch_pending.store(false, std::memory_order_release);
            EmitFailure(I18n::T("网络切换失败"), I18n::T("无法启动切换任务"));
        }
    }

    int ParseSimSlot(const std::string& response) const {
        constexpr char kKey[] = "\"SimSlot\"";
        std::size_t position = 0;
        while ((position = response.find(kKey, position)) != std::string::npos) {
            const std::size_t comma = response.find(',', position);
            if (comma == std::string::npos) return -1;
            std::size_t cursor = comma + 1;
            while (cursor < response.size() &&
                   std::isspace(static_cast<unsigned char>(response[cursor]))) {
                ++cursor;
            }
            if (cursor >= response.size() ||
                !std::isdigit(static_cast<unsigned char>(response[cursor]))) {
                position = comma + 1;
                continue;
            }
            int slot = 0;
            while (cursor < response.size() &&
                   std::isdigit(static_cast<unsigned char>(response[cursor]))) {
                slot = slot * 10 + (response[cursor] - '0');
                ++cursor;
            }
            return slot;
        }
        return -1;
    }

    static void SimQueryTaskEntry(void* arg) {
        auto* self = static_cast<Impl*>(arg);
        if (self == nullptr || !self->active.load(std::memory_order_acquire)) {
            vTaskDelete(nullptr);
            return;
        }
        auto* modem = self->GetModemBoard();
        if (modem != nullptr && self->active.load(std::memory_order_acquire)) {
            std::string response;
            if (modem->SendAtCommand("AT+ECSIMCFG?", response, 5000, true) == ESP_OK) {
                const int slot = self->ParseSimSlot(response);
                if (slot == kSimExternal || slot == kSimInternal) {
                    Event event;
                    event.type = EventType::SimSlotSynced;
                    event.value = slot;
                    self->Emit(event);
                }
            }
        }
        vTaskDelete(nullptr);
    }

    void QuerySimSlot() {
        if (!active.load(std::memory_order_acquire)) return;
        if (GetModemBoard() == nullptr) return;
        xTaskCreate(SimQueryTaskEntry, "sim_query", 4096, this, 5, nullptr);
    }

    struct SimSwitchContext {
        Impl* adapter;
        int target_slot;
    };

    static void SimSwitchTaskEntry(void* arg) {
        std::unique_ptr<SimSwitchContext> context(static_cast<SimSwitchContext*>(arg));
        context->adapter->SimSwitchTask(*context);
    }

    void SimSwitchTask(const SimSwitchContext& context) {
        if (!active.load(std::memory_order_acquire)) {
            sim_switch_pending.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }
        Nt26Board* modem = GetModemBoard();
        if (modem == nullptr) {
            sim_switch_pending.store(false, std::memory_order_release);
            EmitFailure(I18n::T("SIM 卡切换失败"), I18n::T("未检测到 4G 模块"));
            vTaskDelete(nullptr);
            return;
        }

        auto send = [modem](const std::string& command, std::string& response,
                            uint32_t timeout) {
            response.clear();
            return modem->SendAtCommand(command, response, timeout, true);
        };
        std::string response;
        EmitStatus(I18n::T("正在关闭射频…\nAT+CFUN=0"), kProgressColor);
        if (send("AT+CFUN=0", response, 8000) != ESP_OK ||
            response.find("OK") == std::string::npos) {
            sim_switch_pending.store(false, std::memory_order_release);
            EmitFailure(I18n::T("SIM 卡切换失败"),
                        I18n::T("AT+CFUN=0 执行失败"));
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));

        char command[64];
        std::snprintf(command, sizeof(command), "AT+ECSIMCFG=SimSlot,%d",
                      context.target_slot);
        char progress[128];
        std::snprintf(progress, sizeof(progress), I18n::T("正在切换到%s…\n%s"),
                      SimSlotName(context.target_slot), command);
        EmitStatus(progress, kProgressColor);
        if (send(command, response, 5000) != ESP_OK ||
            response.find("OK") == std::string::npos) {
            std::string ignored;
            send("AT+CFUN=1", ignored, 10000);
            sim_switch_pending.store(false, std::memory_order_release);
            EmitFailure(I18n::T("SIM 卡切换失败"),
                        I18n::T("AT+ECSIMCFG 执行失败"));
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        EmitStatus(I18n::T("正在重新搜网…\nAT+CFUN=1"), kProgressColor);
        send("AT+CFUN=1", response, 15000);
        SaveSimSlot(context.target_slot);
        sim_switch_pending.store(false, std::memory_order_release);
        Event slot;
        slot.type = EventType::SimSlotSynced;
        slot.value = context.target_slot;
        Emit(slot);
        std::snprintf(progress, sizeof(progress), I18n::T("已切换到%s"),
                      SimSlotName(context.target_slot));
        BeginRestartCountdown(progress, true);
        vTaskDelete(nullptr);
    }

    void SwitchSimSlot(int slot) {
        if (!active.load(std::memory_order_acquire)) return;
        if (slot != kSimExternal && slot != kSimInternal) return;
        if (!IsCellularMode()) {
            SaveSimSlot(slot);
            SwitchNetwork();
            return;
        }
        if (GetSavedSimSlot() == slot) return;
        if (sim_switch_pending.exchange(true, std::memory_order_acq_rel)) return;
        Event event;
        event.type = EventType::SimSwitchStarted;
        event.value = slot;
        char title[128];
        std::snprintf(title, sizeof(title), I18n::T("正在切换到%s…"),
                      SimSlotName(slot));
        event.text = title;
        Emit(event);
        auto* context = new SimSwitchContext{this, slot};
        if (xTaskCreate(SimSwitchTaskEntry, "sim_switch", 4096, context, 5, nullptr) !=
            pdPASS) {
            delete context;
            sim_switch_pending.store(false, std::memory_order_release);
            EmitFailure(I18n::T("SIM 卡切换失败"),
                        I18n::T("无法启动 SIM 切换任务"));
        }
    }

    void Start() {
        if (active.exchange(true, std::memory_order_acq_rel)) return;
        EmitModeSnapshot();
        EmitSavedNetworks();
        EmitNearbyNetworks(false, false);
        if (IsCellularMode()) {
            QuerySimSlot();
        } else {
            EmitStatus("", kSubtleColor);
        }
    }

    void Stop() {
        if (!active.exchange(false, std::memory_order_acq_rel)) return;
        if (events != nullptr) {
            xEventGroupSetBits(events, kScanDone | kConnected | kDisconnected);
        }
        TeardownWifi();
        scan_in_progress.store(false, std::memory_order_release);
        connect_in_progress.store(false, std::memory_order_release);
        network_switch_pending.store(false, std::memory_order_release);
        sim_switch_pending.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(scan_results_mutex);
            scan_results.clear();
        }
    }

    void Execute(const Command& command) {
        switch (command.type) {
            case CommandType::Start:
                Start();
                break;
            case CommandType::Stop:
                Stop();
                break;
            case CommandType::Scan:
                StartScan();
                break;
            case CommandType::ConnectSaved:
                ConnectSaved(command.index);
                break;
            case CommandType::Connect:
                StartConnect(command.ssid, command.password);
                break;
            case CommandType::SwitchNetwork:
                SwitchNetwork();
                break;
            case CommandType::QuerySimSlot:
                QuerySimSlot();
                break;
            case CommandType::SwitchSimSlot:
                SwitchSimSlot(command.value);
                break;
        }
    }
};

Adapter::Adapter() : impl_(new Impl) {}

Adapter::~Adapter() {
    if (impl_ != nullptr) {
        impl_->Stop();
        delete impl_;
        impl_ = nullptr;
    }
}

void Adapter::SetEventSink(EventSink sink) {
    if (impl_ != nullptr) impl_->sink = std::move(sink);
}

void Adapter::Execute(const Command& command) {
    if (impl_ != nullptr) impl_->Execute(command);
}

}  // namespace agent_ui::network
