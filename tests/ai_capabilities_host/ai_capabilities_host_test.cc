#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <atomic>

#include "ai/ai_availability.h"
#include "ai/ai_capabilities.h"

namespace {

ai::CapabilityProvider MakeProvider(const std::string& id,
                                    const std::string& operation_id,
                                    std::map<std::string, bool>* cancelled) {
    return {
        .descriptor = {id, "Host test", "Registry host test", "{\"type\":\"object\"}"},
        .get_state = [] { return ai::CapabilityState{}; },
        .invoke = [operation_id](const ai::InvokeRequest&) {
            return ai::OperationResult{.operation_id = operation_id,
                                       .status = ai::OperationStatus::Pending};
        },
        .get_result = [](const std::string&) {
            return ai::OperationResult{.status = ai::OperationStatus::Pending};
        },
        .cancel = [cancelled](const std::string& operation_id) {
            (*cancelled)[operation_id] = true;
            return true;
        },
    };
}

}  // namespace

int main() {
    auto& registry = ai::CapabilityRegistry::Get();
    registry.ResetForTest();
    ai::Availability::Get().ReleaseOwner("host-test");

    std::map<std::string, bool> cancelled;
    std::string error;
    assert(registry.Register(MakeProvider("host.pending", "pending-1", &cancelled), &error));
    assert(!registry.Register(MakeProvider("host.pending", "pending-1", &cancelled), &error));

    ai::InvokeRequest request{.capability_id = "host.pending", .arguments_json = "{}"};
    const ai::OperationResult pending = registry.Invoke(request);
    assert(pending.status == ai::OperationStatus::Pending);
    assert(!pending.operation_id.empty());

    assert(registry.Unregister("host.pending", &error));
    assert(cancelled["pending-1"]);
    assert(registry.GetResult(pending.operation_id).status == ai::OperationStatus::Cancelled);
    assert(registry.Unregister("host.pending", &error));

    assert(registry.Register(MakeProvider("host.gated", "pending-2", &cancelled), &error));
    const auto token = ai::Availability::Get().AcquireBlock("host-test", "test gate");
    assert(token != 0);
    assert(registry.Invoke({.capability_id = "host.gated", .arguments_json = "{}"}).status ==
           ai::OperationStatus::Failed);
    assert(ai::Availability::Get().ReleaseBlock(token));
    assert(registry.Unregister("host.gated", &error));

    assert(registry.Register(MakeProvider("host.idempotent", "pending-3", &cancelled), &error));
    const ai::InvokeRequest first{.capability_id = "host.idempotent",
                                  .arguments_json = "{}",
                                  .request_id = "same-request"};
    const ai::OperationResult first_pending = registry.Invoke(first);
    assert(first_pending.status == ai::OperationStatus::Pending);
    assert(registry.Invoke(first).operation_id == first_pending.operation_id);
    assert(registry.Invoke({.capability_id = "host.idempotent",
                            .arguments_json = "{\"changed\":true}",
                            .request_id = "same-request"}).status == ai::OperationStatus::Failed);
    assert(registry.Invoke({.capability_id = "host.idempotent",
                            .arguments_json = "{} trailing"}).status == ai::OperationStatus::Failed);
    std::string too_deep = "";
    for (int index = 0; index < 17; ++index) too_deep += "{\"x\":";
    too_deep += "0";
    for (int index = 0; index < 17; ++index) too_deep += "}";
    assert(registry.Invoke({.capability_id = "host.idempotent",
                            .arguments_json = too_deep}).status == ai::OperationStatus::Failed);
    assert(registry.Cancel(first_pending.operation_id, &error));
    assert(registry.GetResult(first_pending.operation_id).status == ai::OperationStatus::Cancelled);
    assert(registry.Unregister("host.idempotent", &error));

    ai::CapabilityProvider capacity_provider = MakeProvider("host.capacity", "unused", &cancelled);
    capacity_provider.invoke = [](const ai::InvokeRequest& request) {
        return ai::OperationResult{.operation_id = "capacity-" + request.request_id,
                                   .status = ai::OperationStatus::Pending};
    };
    assert(registry.Register(std::move(capacity_provider), &error));
    std::string first_capacity_operation;
    for (int index = 0; index < 16; ++index) {
        const auto capacity_result = registry.Invoke({.capability_id = "host.capacity",
                                                      .arguments_json = "{}",
                                                      .request_id = "capacity-" + std::to_string(index)});
        assert(capacity_result.status == ai::OperationStatus::Pending);
        if (index == 0) first_capacity_operation = capacity_result.operation_id;
    }
    assert(registry.Invoke({.capability_id = "host.capacity",
                            .arguments_json = "{}",
                            .request_id = "capacity-overflow"}).status ==
           ai::OperationStatus::Failed);
    assert(registry.Unregister("host.capacity", &error));
    assert(registry.GetResult(first_capacity_operation).status == ai::OperationStatus::Cancelled);

    std::atomic<int> invokes{0};
    ai::CapabilityProvider concurrent = MakeProvider("host.concurrent", "provider-pending", &cancelled);
    concurrent.invoke = [&invokes](const ai::InvokeRequest&) {
        ++invokes;
        std::this_thread::yield();
        return ai::OperationResult{.operation_id = "provider-pending", .status = ai::OperationStatus::Pending};
    };
    assert(registry.Register(std::move(concurrent), &error));
    ai::OperationResult left;
    ai::OperationResult right;
    ai::InvokeRequest duplicate{.capability_id = "host.concurrent", .arguments_json = "{}", .request_id = "concurrent"};
    std::thread first_thread([&] { left = registry.Invoke(duplicate); });
    std::thread second_thread([&] { right = registry.Invoke(duplicate); });
    first_thread.join();
    second_thread.join();
    assert(invokes == 1);
    assert(left.operation_id == right.operation_id);
    assert(registry.Unregister("host.concurrent", &error));

    ai::CapabilityProvider throwing = MakeProvider("host.throwing", "unused", &cancelled);
    throwing.invoke = [](const ai::InvokeRequest&) -> ai::OperationResult { throw std::runtime_error("expected invoke error"); };
    assert(registry.Register(std::move(throwing), &error));
    assert(registry.Invoke({.capability_id = "host.throwing", .arguments_json = "{}"}).status == ai::OperationStatus::Failed);
    assert(registry.Unregister("host.throwing", &error));

    ai::CapabilityProvider schema = MakeProvider("host.schema", "schema-pending", &cancelled);
    schema.descriptor.args_schema_json =
        "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"on\"]},\"level\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2}},\"required\":[\"mode\"],\"additionalProperties\":false}";
    assert(registry.Register(std::move(schema), &error));
    assert(registry.Invoke({.capability_id = "host.schema", .arguments_json = "{\"mode\":\"off\"}"}).status == ai::OperationStatus::Failed);
    assert(registry.Invoke({.capability_id = "host.schema", .arguments_json = "{\"mode\":\"on\",\"extra\":true}"}).status == ai::OperationStatus::Failed);
    assert(registry.Invoke({.capability_id = "host.schema", .arguments_json = "{\"mode\":\"on\",\"level\":2}"}).status == ai::OperationStatus::Pending);
    assert(registry.Unregister("host.schema", &error));

    std::atomic<bool> complete{false};
    ai::CapabilityProvider reclaim = MakeProvider("host.reclaim", "unused", &cancelled);
    reclaim.invoke = [&complete](const ai::InvokeRequest& request) {
        return ai::OperationResult{.operation_id = "reclaim-" + request.request_id,
                                   .status = ai::OperationStatus::Pending};
    };
    reclaim.get_result = [&complete](const std::string&) {
        return complete.load() ? ai::OperationResult{.status = ai::OperationStatus::Succeeded}
                               : ai::OperationResult{.status = ai::OperationStatus::Pending};
    };
    assert(registry.Register(std::move(reclaim), &error));
    for (int index = 0; index < 16; ++index) {
        assert(registry.Invoke({.capability_id = "host.reclaim", .arguments_json = "{}",
                                .request_id = "reclaim-" + std::to_string(index)}).status == ai::OperationStatus::Pending);
    }
    complete = true;
    assert(registry.Invoke({.capability_id = "host.reclaim", .arguments_json = "{}",
                            .request_id = "reclaimed"}).status == ai::OperationStatus::Pending);
    assert(registry.Unregister("host.reclaim", &error));

    std::cout << "ai capability registry host test passed\n";
}
