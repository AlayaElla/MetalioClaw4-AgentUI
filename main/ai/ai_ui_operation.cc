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
    UiOperations::CancellableCancel cancellable_cancel;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> cancel_attempt_active{false};
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
    UiOperations::Step step;
    std::function<void()> legacy_cancel;
    UiOperations::CancellableCancel cancellable_cancel;
    UiOperations::Step released_step;
    std::function<void()> released_cancel;
    UiOperations::CancellableCancel released_cancellable_cancel;
    bool already_cancelled = false;
    bool expired = false;
    bool unavailable = false;
    bool started = false;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (job->result.status != OperationStatus::Pending) {
            lv_timer_delete(timer);
            delete holder;
            return;
        }
        already_cancelled = job->cancelled.load();
        expired = std::chrono::steady_clock::now() >= job->deadline;
        started = job->started;
        if (!already_cancelled && (!expired || job->cancellable_cancel) &&
            !started && (!Availability::Get().IsAvailable() ||
                         Availability::Get().Generation() != job->generation)) {
            unavailable = true;
        } else if (!already_cancelled && (!expired || job->cancellable_cancel)) {
            job->started = true;
            step = job->step;
        }
        legacy_cancel = job->cancel;
        cancellable_cancel = job->cancellable_cancel;
    }

    if (already_cancelled) {
        if (started && legacy_cancel) legacy_cancel();
        result.status = OperationStatus::Cancelled;
        result.error = "Operation cancelled; completed effects are not undone";
    } else if (expired && cancellable_cancel) {
        // Destructive work that already started must keep reporting its real
        // outcome. A false response means cancellation lost the start race.
        bool accepted = false;
        try { accepted = cancellable_cancel(); } catch (...) {}
        if (accepted) {
            std::lock_guard<std::mutex> lock(job->mutex);
            if (job->result.status == OperationStatus::Pending) {
                job->cancelled.store(true);
                result.status = OperationStatus::Cancelled;
                result.error = "Operation cancelled before it started";
            }
        } else {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->deadline = std::chrono::steady_clock::now() +
                           std::chrono::seconds(1);
        }
        if (result.status == OperationStatus::Pending && step) {
            try { result = step(); }
            catch (const std::exception& error) {
                result.status = OperationStatus::Failed;
                result.error = error.what();
            } catch (...) { result = Error("UI operation failed unexpectedly"); }
        }
    } else if (expired) {
        if (started && legacy_cancel) legacy_cancel();
        result = Error("Operation timed out");
    } else if (unavailable) {
        result = Error("AI availability changed before execution");
    } else {
        try {
            if (step) result = step();
            else result = Error("UI operation has no active step");
        } catch (const std::exception& error) {
            result.status = OperationStatus::Failed;
            result.error = error.what();
        } catch (...) {
            result = Error("UI operation failed unexpectedly");
        }
    }
    bool terminal = false;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (result.status == OperationStatus::Pending && job->cancelled.load()) {
            result.status = OperationStatus::Cancelled;
            result.error = "Operation cancelled; completed effects are not undone";
        }
        result.operation_id = job->result.operation_id;
        result.generation = job->generation;
        job->result = result;
        terminal = result.status != OperationStatus::Pending;
        if (terminal) {
            released_step.swap(job->step);
            released_cancel.swap(job->cancel);
            released_cancellable_cancel.swap(job->cancellable_cancel);
        }
    }
    if (terminal) {
        lv_timer_delete(timer);
        delete holder;
    }
}

OperationResult SubmitInternal(const InvokeRequest& request,
                               UiOperations::Step step,
                               std::function<void()> cancel,
                               UiOperations::CancellableCancel cancellable_cancel) {
    if (!step) return Error("Missing UI operation");
    auto job = std::make_shared<Job>();
    job->generation = request.generation;
    job->step = std::move(step);
    job->cancel = std::move(cancel);
    job->cancellable_cancel = std::move(cancellable_cancel);
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
}  // namespace

OperationResult UiOperations::Submit(const InvokeRequest& request, Step step,
                                      std::function<void()> cancel) {
    return SubmitInternal(request, std::move(step), std::move(cancel), {});
}

OperationResult UiOperations::SubmitWithCancellableCancel(
    const InvokeRequest& request, Step step, CancellableCancel cancel) {
    if (!cancel) return Error("Missing cancellable cancel callback");
    return SubmitInternal(request, std::move(step), {}, std::move(cancel));
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
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex);
        const auto found = jobs.find(id);
        if (found == jobs.end()) return false;
        job = found->second;
    }
    UiOperations::CancellableCancel try_cancel;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (job->result.status != OperationStatus::Pending) return false;
        try_cancel = job->cancellable_cancel;
        if (!try_cancel) {
            job->cancelled.store(true);
            return true;
        }
    }
    bool expected = false;
    if (!job->cancel_attempt_active.compare_exchange_strong(expected, true)) return false;
    bool accepted = false;
    try { accepted = try_cancel(); } catch (...) { accepted = false; }
    job->cancel_attempt_active.store(false);
    if (!accepted) return false;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (job->result.status != OperationStatus::Pending) return false;
        job->cancelled.store(true);
    }
    return true;
}
}  // namespace ai
