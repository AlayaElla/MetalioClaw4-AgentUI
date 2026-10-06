#pragma once

#include <utility>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace agent_ui {

// Serializes background standby-rail entry against prepared wake transitions.
// FreeRTOS binary semaphores have no task-owner restriction, so a prepared
// Lease may safely be released by the LVGL task after it applies or discards
// the queued wake callback. The UI owner never waits for this gate.
class PowerTransitionCoordinator {
public:
    class Lease {
    public:
        Lease() = default;
        explicit Lease(SemaphoreHandle_t semaphore) : semaphore_(semaphore) {}
        ~Lease() { Release(); }
        Lease(Lease&& other) noexcept
            : semaphore_(std::exchange(other.semaphore_, nullptr)) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                Release();
                semaphore_ = std::exchange(other.semaphore_, nullptr);
            }
            return *this;
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        bool owns_lock() const { return semaphore_ != nullptr; }

    private:
        void Release() {
            if (semaphore_ != nullptr) {
                (void)xSemaphoreGive(semaphore_);
                semaphore_ = nullptr;
            }
        }
        SemaphoreHandle_t semaphore_ = nullptr;
    };

    static Lease Acquire() {
        SemaphoreHandle_t semaphore = Semaphore();
        if (semaphore == nullptr ||
            xSemaphoreTake(semaphore, portMAX_DELAY) != pdTRUE) return {};
        return Lease(semaphore);
    }

    static Lease TryAcquire() {
        SemaphoreHandle_t semaphore = Semaphore();
        if (semaphore == nullptr || xSemaphoreTake(semaphore, 0) != pdTRUE)
            return {};
        return Lease(semaphore);
    }

private:
    static SemaphoreHandle_t Semaphore() {
        static StaticSemaphore_t storage;
        static SemaphoreHandle_t semaphore =
            xSemaphoreCreateBinaryStatic(&storage);
        static const bool seeded =
            semaphore != nullptr && xSemaphoreGive(semaphore) == pdTRUE;
        (void)seeded;
        return semaphore;
    }
};

}  // namespace agent_ui
