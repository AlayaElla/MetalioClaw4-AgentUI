#include "ai_availability.h"

#include <utility>

namespace ai {

Availability& Availability::Get() {
    static Availability instance;
    return instance;
}

Availability::Snapshot Availability::SnapshotLocked() const {
    Snapshot result{blocks_.empty(), generation_, {}};
    for (const auto& entry : blocks_) result.reasons.push_back(entry.second.reason);
    return result;
}

Availability::Token Availability::AcquireBlock(const std::string& owner,
                                               const std::string& reason) {
    Observer notify;
    Snapshot snapshot;
    Token token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        do { token = ++next_token_; } while (token == 0 || blocks_.count(token));
        blocks_.emplace(token, Block{owner, reason.empty() ? owner : reason});
        ++generation_;
        snapshot = SnapshotLocked();
        notify = observer_;
    }
    if (notify) notify(snapshot);
    return token;
}

bool Availability::ReleaseBlock(Token token) {
    Observer notify;
    Snapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token == 0 || blocks_.erase(token) == 0) return false;
        ++generation_;
        snapshot = SnapshotLocked();
        notify = observer_;
    }
    if (notify) notify(snapshot);
    return true;
}

void Availability::ReleaseOwner(const std::string& owner) {
    Observer notify;
    Snapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool changed = false;
        for (auto it = blocks_.begin(); it != blocks_.end();) {
            if (it->second.owner == owner) { it = blocks_.erase(it); changed = true; }
            else ++it;
        }
        if (!changed) return;
        ++generation_;
        snapshot = SnapshotLocked();
        notify = observer_;
    }
    if (notify) notify(snapshot);
}

bool Availability::IsAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocks_.empty();
}

uint64_t Availability::Generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

Availability::Snapshot Availability::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SnapshotLocked();
}

void Availability::SetObserver(Observer observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observer_ = std::move(observer);
}

}  // namespace ai
