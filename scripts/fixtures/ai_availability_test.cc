#include "ai/ai_availability.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    auto& policy = ai::Availability::Get();
    std::atomic<unsigned> observed{0};
    policy.SetObserver([&](const auto&) {
        // Observers may query the policy without recursively taking its lock.
        (void)policy.GetSnapshot();
        ++observed;
    });
    assert(policy.IsAvailable());
    const auto epoch = policy.Generation();
    const auto call = policy.AcquireBlock("call", "通话");
    const auto recording = policy.AcquireBlock("recording", "录音");
    assert(call != 0 && call != recording);
    assert(!policy.IsAvailable() && policy.GetSnapshot().reasons.size() == 2);
    assert(policy.ReleaseBlock(call));
    assert(!policy.ReleaseBlock(call));
    assert(!policy.IsAvailable());
    policy.ReleaseOwner("missing");
    policy.ReleaseOwner("recording");
    assert(policy.IsAvailable() && !policy.ReleaseBlock(recording));
    assert(policy.Generation() > epoch && observed == 4);
    const auto persistent = policy.AcquireBlock("parent");
    std::vector<std::thread> workers;
    for (unsigned n = 0; n < 4; ++n) workers.emplace_back([&, n]() {
        for (unsigned i = 0; i < 200; ++i) {
            auto token = policy.AcquireBlock("worker-" + std::to_string(n));
            assert(!policy.IsAvailable());
            assert(policy.ReleaseBlock(token));
        }
    });
    for (auto& worker : workers) worker.join();
    assert(!policy.IsAvailable() && policy.GetSnapshot().reasons.size() == 1);
    assert(policy.ReleaseBlock(persistent));
    assert(policy.IsAvailable());
    policy.SetObserver({});
    std::cout << "AI availability: leases, ownership, stale release, observer and concurrency passed\n";
}
