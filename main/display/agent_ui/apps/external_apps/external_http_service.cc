#include "external_http_service.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace agent_ui::external_apps {
namespace {

constexpr char kTag[] = "ExternalHttp";
constexpr size_t kMaxUrlBytes = 1024;
constexpr size_t kMaxBodyBytes = 16 * 1024;
constexpr size_t kMaxContentTypeBytes = 96;
constexpr uint32_t kDefaultTimeoutMs = 10000;
constexpr uint32_t kMinimumTimeoutMs = 1000;
constexpr uint32_t kMaximumTimeoutMs = 30000;
constexpr uint32_t kDefaultResponseBytes = 32 * 1024;
constexpr uint32_t kMaximumResponseBytes = 64 * 1024;
constexpr size_t kMaximumOwnerRequests = 2;
HttpService* s_existing_service = nullptr;

bool IsSupportedUrl(const char* url) {
    if (url == nullptr) return false;
    const size_t size = strnlen(url, kMaxUrlBytes + 1);
    if (size == 0 || size > kMaxUrlBytes) return false;
    return strncasecmp(url, "http://", 7) == 0 ||
           strncasecmp(url, "https://", 8) == 0;
}

bool IsBoundedText(const char* text, size_t maximum) {
    return text == nullptr || strnlen(text, maximum + 1) <= maximum;
}

}  // namespace

struct HttpService::Impl {
    struct Request {
        uint32_t id = 0;
        void* owner = nullptr;
        metalio_app_http_method_t method = METALIO_APP_HTTP_GET;
        std::string url;
        std::string body;
        std::string content_type;
        uint32_t timeout_ms = kDefaultTimeoutMs;
        uint32_t maximum_response_bytes = kDefaultResponseBytes;
        metalio_app_http_callback_t callback = nullptr;
        void* app_context = nullptr;
        std::atomic<bool> cancelled{false};
        int32_t status_code = 0;
        metalio_app_http_error_t error = METALIO_APP_HTTP_OK;
        std::vector<uint8_t> response;
        bool truncated = false;
    };

    SemaphoreHandle_t mutex = nullptr;
    uint32_t next_id = 1;
    std::vector<std::shared_ptr<Request>> requests;
    std::vector<std::shared_ptr<Request>*> deliveries;
    std::atomic<uint32_t> active_workers{0};
    std::atomic<bool> reset_in_progress{false};

    bool Lock(TickType_t timeout = pdMS_TO_TICKS(1000)) const {
        return mutex != nullptr &&
               xSemaphoreTakeRecursive(mutex, timeout) == pdTRUE;
    }

    void Unlock() const {
        if (mutex != nullptr) xSemaphoreGiveRecursive(mutex);
    }

    void Remove(const std::shared_ptr<Request>& request) {
        if (!Lock()) return;
        requests.erase(std::remove(requests.begin(), requests.end(), request),
                       requests.end());
        Unlock();
    }

    static void DeliverThunk(void* argument) {
        HttpService::Get().impl_->Deliver(
            static_cast<std::shared_ptr<Request>*>(argument));
    }

    void Complete(const std::shared_ptr<Request>& request) {
        if (request->cancelled.load(std::memory_order_acquire) ||
            reset_in_progress.load(std::memory_order_acquire)) {
            Remove(request);
            return;
        }
        auto* holder =
            new (std::nothrow) std::shared_ptr<Request>(request);
        if (holder == nullptr) {
            Remove(request);
            return;
        }
        if (!Lock(pdMS_TO_TICKS(2000))) {
            delete holder;
            return;
        }
        const bool cancelled =
            request->cancelled.load(std::memory_order_acquire) ||
            reset_in_progress.load(std::memory_order_acquire) ||
            std::find(requests.begin(), requests.end(), request) ==
                requests.end();
        if (cancelled || lv_async_call(DeliverThunk, holder) != LV_RESULT_OK) {
            requests.erase(std::remove(requests.begin(), requests.end(),
                                       request),
                           requests.end());
            Unlock();
            delete holder;
            return;
        }
        deliveries.push_back(holder);
        Unlock();
    }

    void Deliver(std::shared_ptr<Request>* raw_holder) {
        if (raw_holder == nullptr) return;
        std::unique_ptr<std::shared_ptr<Request>> holder(raw_holder);
        if (!Lock(pdMS_TO_TICKS(2000))) return;
        deliveries.erase(std::remove(deliveries.begin(), deliveries.end(),
                                     raw_holder),
                         deliveries.end());
        const std::shared_ptr<Request>& request = *holder;
        const auto found =
            std::find(requests.begin(), requests.end(), request);
        if (found == requests.end()) {
            Unlock();
            return;
        }
        if (!request->cancelled.load(std::memory_order_acquire) &&
            request->callback != nullptr) {
            const metalio_app_http_response_t response = {
                .request_id = request->id,
                .status_code = request->status_code,
                .error = request->error,
                .body = request->response.empty()
                            ? nullptr
                            : request->response.data(),
                .body_size =
                    static_cast<uint32_t>(request->response.size()),
                .truncated = static_cast<uint8_t>(request->truncated),
                .reserved = {},
            };
            // The recursive mutex keeps Runtime::Unload from invalidating the
            // external callback while it is running. Calls back into this
            // service from the App remain legal.
            request->callback(request->app_context, &response);
        }
        requests.erase(std::remove(requests.begin(), requests.end(), request),
                       requests.end());
        Unlock();
    }

    static void WorkerEntry(void* argument) {
        std::unique_ptr<std::shared_ptr<Request>> holder(
            static_cast<std::shared_ptr<Request>*>(argument));
        Impl* impl = HttpService::Get().impl_;
        if (holder != nullptr && *holder != nullptr) {
            impl->Worker(*holder);
        }
        impl->active_workers.fetch_sub(1, std::memory_order_acq_rel);
        vTaskDelete(nullptr);
    }

    void Worker(const std::shared_ptr<Request>& request) {
        if (request->cancelled.load(std::memory_order_acquire)) {
            request->error = METALIO_APP_HTTP_ERROR_CANCELLED;
            Complete(request);
            return;
        }
        NetworkInterface* network = Board::GetInstance().GetNetwork();
        if (network == nullptr || !Board::GetInstance().IsNetworkConnected()) {
            request->error = METALIO_APP_HTTP_ERROR_NETWORK;
            Complete(request);
            return;
        }
        auto http = network->CreateHttp(2);
        if (http == nullptr) {
            request->error = METALIO_APP_HTTP_ERROR_CREATE;
            Complete(request);
            return;
        }
        http->SetTimeout(static_cast<int>(request->timeout_ms));
        http->SetKeepAlive(false);
        http->SetHeader("Accept", "application/json, text/plain, */*");
        http->SetHeader("Connection", "close");
        if (request->method == METALIO_APP_HTTP_POST) {
            if (!request->content_type.empty()) {
                http->SetHeader("Content-Type", request->content_type);
            }
            http->SetContent(std::string(request->body));
        }
        const char* method =
            request->method == METALIO_APP_HTTP_POST ? "POST" : "GET";
        if (!http->Open(method, request->url)) {
            request->error = METALIO_APP_HTTP_ERROR_OPEN;
            http->Close();
            Complete(request);
            return;
        }

        request->status_code = http->GetStatusCode();
        uint8_t buffer[1024];
        while (!request->cancelled.load(std::memory_order_acquire)) {
            const int count =
                http->Read(reinterpret_cast<char*>(buffer), sizeof(buffer));
            if (count == 0) break;
            if (count < 0) {
                request->error = METALIO_APP_HTTP_ERROR_READ;
                break;
            }
            const size_t remaining =
                request->maximum_response_bytes - request->response.size();
            const size_t copy = std::min<size_t>(remaining, count);
            request->response.insert(request->response.end(), buffer,
                                     buffer + copy);
            if (copy < static_cast<size_t>(count) || remaining == copy) {
                request->truncated = true;
                break;
            }
        }
        http->Close();
        if (request->cancelled.load(std::memory_order_acquire)) {
            request->error = METALIO_APP_HTTP_ERROR_CANCELLED;
            request->response.clear();
        }
        Complete(request);
    }
};

HttpService::HttpService() : impl_(new Impl()) {
    s_existing_service = this;
    impl_->mutex = xSemaphoreCreateRecursiveMutex();
}

HttpService& HttpService::Get() {
    static HttpService service;
    return service;
}

HttpService* HttpService::Existing() {
    return s_existing_service;
}

int HttpService::Start(void* owner,
                       const metalio_app_http_request_t* input,
                       metalio_app_http_callback_t callback,
                       void* app_context, uint32_t* request_id) {
    if (impl_ == nullptr ||
        impl_->reset_in_progress.load(std::memory_order_acquire) ||
        owner == nullptr || input == nullptr ||
        callback == nullptr || request_id == nullptr ||
        !IsSupportedUrl(input->url) ||
        (input->method != METALIO_APP_HTTP_GET &&
         input->method != METALIO_APP_HTTP_POST) ||
        input->body_size > kMaxBodyBytes ||
        (input->body_size > 0 && input->body == nullptr) ||
        !IsBoundedText(input->content_type, kMaxContentTypeBytes)) {
        return -1;
    }
    auto request = std::make_shared<Impl::Request>();
    request->owner = owner;
    request->method = input->method;
    request->url = input->url;
    if (input->body_size > 0) {
        request->body.assign(input->body, input->body + input->body_size);
    }
    if (input->content_type != nullptr) {
        request->content_type = input->content_type;
    }
    request->timeout_ms = std::clamp(
        input->timeout_ms == 0 ? kDefaultTimeoutMs : input->timeout_ms,
        kMinimumTimeoutMs, kMaximumTimeoutMs);
    request->maximum_response_bytes = std::clamp(
        input->max_response_bytes == 0 ? kDefaultResponseBytes
                                       : input->max_response_bytes,
        UINT32_C(1), kMaximumResponseBytes);
    request->callback = callback;
    request->app_context = app_context;

    if (!impl_->Lock()) return -1;
    const size_t owner_count = static_cast<size_t>(std::count_if(
        impl_->requests.begin(), impl_->requests.end(),
        [owner](const std::shared_ptr<Impl::Request>& active) {
            return active != nullptr && active->owner == owner;
        }));
    if (owner_count >= kMaximumOwnerRequests) {
        impl_->Unlock();
        return -2;
    }
    request->id = impl_->next_id++;
    if (request->id == 0) request->id = impl_->next_id++;
    impl_->requests.push_back(request);
    impl_->Unlock();

    auto* holder =
        new (std::nothrow) std::shared_ptr<Impl::Request>(request);
    if (holder == nullptr) {
        impl_->Remove(request);
        return -1;
    }
    impl_->active_workers.fetch_add(1, std::memory_order_acq_rel);
    if (xTaskCreate(Impl::WorkerEntry, "external_http", 8192, holder, 4,
                    nullptr) != pdPASS) {
        impl_->active_workers.fetch_sub(1, std::memory_order_acq_rel);
        delete holder;
        impl_->Remove(request);
        return -1;
    }
    *request_id = request->id;
    return 0;
}

int HttpService::Cancel(void* owner, uint32_t request_id) {
    if (impl_ == nullptr || owner == nullptr || request_id == 0 ||
        !impl_->Lock()) {
        return -1;
    }
    const auto found = std::find_if(
        impl_->requests.begin(), impl_->requests.end(),
        [owner, request_id](const std::shared_ptr<Impl::Request>& request) {
            return request != nullptr && request->owner == owner &&
                   request->id == request_id;
        });
    if (found == impl_->requests.end()) {
        impl_->Unlock();
        return -1;
    }
    (*found)->cancelled.store(true, std::memory_order_release);
    (*found)->callback = nullptr;
    (*found)->app_context = nullptr;
    impl_->requests.erase(found);
    impl_->Unlock();
    return 0;
}

void HttpService::CancelOwner(void* owner) {
    if (impl_ == nullptr || owner == nullptr || !impl_->Lock()) return;
    for (const auto& request : impl_->requests) {
        if (request == nullptr || request->owner != owner) continue;
        request->cancelled.store(true, std::memory_order_release);
        request->callback = nullptr;
        request->app_context = nullptr;
    }
    impl_->requests.erase(
        std::remove_if(
            impl_->requests.begin(), impl_->requests.end(),
            [owner](const std::shared_ptr<Impl::Request>& request) {
                return request != nullptr && request->owner == owner;
            }),
        impl_->requests.end());
    impl_->Unlock();
}

bool HttpService::ResetForAppLaunch(uint32_t timeout_ms) {
    if (impl_ == nullptr) return true;
    impl_->reset_in_progress.store(true, std::memory_order_release);
    if (!impl_->Lock(pdMS_TO_TICKS(2000))) {
        impl_->reset_in_progress.store(false, std::memory_order_release);
        return false;
    }
    for (const auto& request : impl_->requests) {
        if (request == nullptr) continue;
        request->cancelled.store(true, std::memory_order_release);
        request->callback = nullptr;
        request->app_context = nullptr;
    }
    impl_->requests.clear();
    for (auto* holder : impl_->deliveries) {
        if (holder == nullptr) continue;
        if (lv_async_call_cancel(Impl::DeliverThunk, holder) == LV_RESULT_OK) {
            delete holder;
        }
    }
    impl_->deliveries.clear();
    impl_->Unlock();

    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (impl_->active_workers.load(std::memory_order_acquire) != 0) {
        if (static_cast<TickType_t>(xTaskGetTickCount() - started) >=
            timeout_ticks) {
            ESP_LOGE(kTag, "HTTP reset timed out with %u worker(s)",
                     static_cast<unsigned>(impl_->active_workers.load(
                         std::memory_order_relaxed)));
            impl_->reset_in_progress.store(false, std::memory_order_release);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    impl_->reset_in_progress.store(false, std::memory_order_release);
    return true;
}

}  // namespace agent_ui::external_apps
