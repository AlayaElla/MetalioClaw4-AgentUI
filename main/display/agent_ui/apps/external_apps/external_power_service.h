#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "metalio_app_api.h"

namespace agent_ui::external_apps {

class PowerService {
public:
    static PowerService& Get();
    static PowerService* Existing();

    int Read(metalio_app_power_reading_t* reading) const;
    int Start(void* owner, uint32_t duration_ms);
    int GetResult(void* owner, metalio_app_standby_result_t* result) const;
    int Cancel(void* owner);
    void SuspendOwner(void* owner);
    void UnloadOwner(void* owner);

private:
    bool Current(uint32_t generation) const;
    void Publish(uint32_t generation, const metalio_app_standby_result_t& result);
    void Run(uint32_t generation, uint32_t duration_ms);
    void WorkerLoop();
    static void Worker(void* context);
    mutable std::mutex mutex_;
    void* owner_ = nullptr;
    void* worker_ = nullptr;
    void* worker_stack_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<uint32_t> generation_{0};
    metalio_app_standby_result_t result_{};
};

}  // namespace agent_ui::external_apps
