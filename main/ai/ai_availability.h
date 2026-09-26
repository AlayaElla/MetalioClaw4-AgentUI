#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ai {

// System-wide device-assistant policy. This does not own the shared audio codec.
// Tokens are unique leases: releasing one caller cannot release another caller.
class Availability {
public:
    using Token = uint64_t;
    struct Snapshot {
        bool available = true;
        uint64_t generation = 0;
        std::vector<std::string> reasons;
    };
    using Observer = std::function<void(const Snapshot&)>;

    static Availability& Get();
    Token AcquireBlock(const std::string& owner, const std::string& reason = {});
    bool ReleaseBlock(Token token);
    void ReleaseOwner(const std::string& owner);
    bool IsAvailable() const;
    uint64_t Generation() const;
    Snapshot GetSnapshot() const;
    // Installed by Application. Invoked outside the policy lock; consumers must
    // re-read current policy before applying work queued on another thread.
    void SetObserver(Observer observer);

private:
    struct Block { std::string owner; std::string reason; };
    Snapshot SnapshotLocked() const;
    mutable std::mutex mutex_;
    std::map<Token, Block> blocks_;
    Token next_token_ = 0;
    uint64_t generation_ = 0;
    Observer observer_;
};

}  // namespace ai
