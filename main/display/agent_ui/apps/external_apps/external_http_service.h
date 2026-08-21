#pragma once

#include <cstdint>

#include "metalio_app_api.h"

namespace agent_ui::external_apps {

class HttpService {
public:
    static HttpService& Get();
    static HttpService* Existing();

    int Start(void* owner, const metalio_app_http_request_t* request,
              metalio_app_http_callback_t callback, void* app_context,
              uint32_t* request_id);
    int Cancel(void* owner, uint32_t request_id);
    void CancelOwner(void* owner);
    // Host-level launch barrier. Cancels requests from all previous external
    // App owners and waits for their worker tasks to leave.
    bool ResetForAppLaunch(uint32_t timeout_ms);

private:
    struct Impl;
    HttpService();
    ~HttpService() = default;
    HttpService(const HttpService&) = delete;
    HttpService& operator=(const HttpService&) = delete;

    Impl* impl_ = nullptr;
};

}  // namespace agent_ui::external_apps
