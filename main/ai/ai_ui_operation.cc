#include "ai_ui_operation.h"
#include "ai_availability.h"
#include "ui_dispatcher.h"
#include "lvgl.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <exception>

namespace ai {
namespace {
struct Job {
    std::mutex mutex;
    OperationResult result;
    UiOperations::Step step;
    std::function<void()> cancel;
    std::atomic<bool> cancelled{false};
    bool started = false;
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point deadline;
};
std::mutex jobs_mutex;
std::map<std::string, std::shared_ptr<Job>> jobs;
uint64_t next_job = 0;

OperationResult Error(const char* message) {
    OperationResult result;
    result.error = message;
    return result;
}

void Tick(lv_timer_t* timer) {
    auto* holder = static_cast<std::shared_ptr<Job>*>(lv_timer_get_user_data(timer));
    auto job = *holder;
    OperationResult result;
    if (job->cancelled.load()) {
        if (job->started && job->cancel) job->cancel();
        result.status = OperationStatus::Cancelled;
        result.error = "Operation cancelled; completed effects are not undone";
    } else if (std::chrono::steady_clock::now() >= job->deadline) {
        if (job->started && job->cancel) job->cancel();
        result = Error("Operation timed out");
    } else if (!job->started && (!Availability::Get().IsAvailable() ||
               Availability::Get().Generation() != job->generation)) {
        result = Error("AI availability changed before execution");
    } else {
        job->started = true;
        try {
            result = job->step();
        } catch (const std::exception& error) {
            result.status = OperationStatus::Failed;
            result.error = error.what();
        } catch (...) {
            result = Error("UI operation failed unexpectedly");
        }
    }
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        result.operation_id = job->result.operation_id;
        result.generation = job->generation;
        job->result = result;
    }
    if (result.status != OperationStatus::Pending) {
        job->step = {};
        job->cancel = {};
        lv_timer_delete(timer);
        delete holder;
    }
}
}  // namespace

OperationResult UiOperations::Submit(const InvokeRequest& request, Step step,
                                      std::function<void()> cancel) {
    if (!step) return Error("Missing UI operation");
    auto job = std::make_shared<Job>();
    job->generation = request.generation;
    job->step = std::move(step);
    job->cancel = std::move(cancel);
    job->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(
        request.deadline_ms ? std::clamp<uint32_t>(request.deadline_ms, 100, 60000) : 30000);
    job->result.status = OperationStatus::Pending;
    job->result.generation = request.generation;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex);
        for (auto it = jobs.begin(); jobs.size() >= 64 && it != jobs.end();) {
            auto candidate = it->second;
            std::lock_guard<std::mutex> result_lock(candidate->mutex);
            if (candidate->result.status != OperationStatus::Pending) it = jobs.erase(it);
            else ++it;
        }
        if (jobs.size() >= 64) return Error("UI operation queue is full");
        job->result.operation_id = "ui-" + std::to_string(++next_job);
        jobs.emplace(job->result.operation_id, job);
    }
    if (!UiDispatcher::Post([job]() {
            auto* holder = new std::shared_ptr<Job>(job);
            auto* timer = lv_timer_create(Tick, 100, holder);
            if (timer) lv_timer_ready(timer);
            else {
                delete holder;
                std::lock_guard<std::mutex> lock(job->mutex);
                job->result.status = OperationStatus::Failed;
                job->result.error = "Cannot allocate UI operation timer";
            }
        })) {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->result.status = OperationStatus::Failed;
        job->result.error = "Device UI is not ready";
    }
    std::lock_guard<std::mutex> lock(job->mutex);
    return job->result;
}

OperationResult UiOperations::GetResult(const std::string& id) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex);
        auto found = jobs.find(id);
        if (found == jobs.end()) return Error("Unknown UI operation");
        job = found->second;
    }
    std::lock_guard<std::mutex> lock(job->mutex);
    return job->result;
}

bool UiOperations::Cancel(const std::string& id) {
    std::lock_guard<std::mutex> lock(jobs_mutex);
    const auto found = jobs.find(id);
    if (found == jobs.end()) return false;
    std::lock_guard<std::mutex> result_lock(found->second->mutex);
    if (found->second->result.status != OperationStatus::Pending) return false;
    found->second->cancelled.store(true);
    return true;
}
}  // namespace ai
