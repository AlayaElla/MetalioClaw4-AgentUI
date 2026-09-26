#include <cassert>
#include <iostream>
#include <string>
#include "ai/ai_capabilities.h"
#include "ai/ai_availability.h"
#include "codex_ai_provider.h"
#include "agent_ui/core/navigation.h"
#include "ui_dispatcher.h"
using namespace ai;
namespace codex = agent_ui::codex_ai;
int main() {
    int sent = 0, cancelled = 0;
    bool ready = false;
    std::string last_id;
    Availability::Token realtime_block = 0;
    codex::Hooks hooks;
    hooks.state = [&] { CapabilityState state; state.available = ready; state.enabled = ready; return state; };
    hooks.dispatch = [&](const std::string& action, const std::string&, const std::string& id) {
        ++sent; last_id = id;
        if (action == "realtime_start") realtime_block = Availability::Get().AcquireBlock("codex.realtime");
        return codex::DispatchResult{true, false, id, "{}", ""};
    };
    hooks.cancel = [&](const std::string&) { ++cancelled; };
    std::string error;
    assert(codex::Register(hooks, &error));
    unsigned serial = 0;
    auto invoke = [&](const char* action) {
        return CapabilityRegistry::Get().Invoke({"codex.control", std::string("{\"action\":\"") + action + "\"}",
                                                "test-" + std::to_string(++serial), 1000, 0});
    };
    auto status = [&](const OperationResult& result) { return CapabilityRegistry::Get().GetResult(result.operation_id).status; };
    auto ack = [&](const char* type, const std::string& extra = "\"success\":true") {
        codex::ObserveMessage(std::string("{\"type\":\"") + type + "\",\"request_id\":\"" + last_id + "\"," + extra + "}");
        ui_test::Tick();
    };
    auto text = invoke("text");
    assert(text.status == OperationStatus::Pending);
    ui_test::RunPosts(); ui_test::Tick();
    assert(agent_ui::Navigation::Get().current() == agent_ui::ScreenId::Codex);
    assert(sent == 0);
    ready = true; ui_test::Tick(); assert(sent == 1);
    ack("turn_stop_result"); assert(status(text) == OperationStatus::Pending);
    ack("codex_text_result", "\"success\":true,\"submissionConfirmed\":false,\"delivery\":\"submitted_to_hid\"");
    assert(status(text) == OperationStatus::Succeeded);
    assert(CapabilityRegistry::Get().GetResult(text.operation_id).result_json.find("submissionConfirmed") != std::string::npos);
    ack("codex_text_result", "\"success\":false"); assert(status(text) == OperationStatus::Succeeded);

    auto call = invoke("realtime_start"); ui_test::RunPosts(); ui_test::Tick();
    assert(!Availability::Get().IsAvailable());
    ack("realtime_status", "\"state\":\"connecting\",\"acceptsAudio\":false");
    assert(status(call) == OperationStatus::Pending);
    ack("realtime_status", "\"state\":\"listening\",\"acceptsAudio\":true");
    assert(status(call) == OperationStatus::Succeeded);
    Availability::Get().ReleaseBlock(realtime_block);
    auto failed_call = invoke("realtime_start"); ui_test::RunPosts(); ui_test::Tick();
    ack("realtime_status", "\"state\":\"error\",\"message\":\"audio failed\"");
    assert(status(failed_call) == OperationStatus::Failed);
    Availability::Get().ReleaseBlock(realtime_block);

    auto setting = invoke("model"); ui_test::RunPosts(); ui_test::Tick();
    ack("codex_action_result", "\"action\":\"set_effort\",\"success\":true");
    assert(status(setting) == OperationStatus::Pending);
    ack("codex_action_result", "\"action\":\"set_model\",\"success\":true");
    assert(status(setting) == OperationStatus::Succeeded);
    auto stop = invoke("stop"); ui_test::RunPosts(); ui_test::Tick();
    assert(CapabilityRegistry::Get().Cancel(stop.operation_id)); ui_test::Tick();
    assert(cancelled == 1 && status(stop) == OperationStatus::Cancelled);
    ack("turn_stop_result"); assert(status(stop) == OperationStatus::Cancelled);

    // A block/unblock cycle while the page opens invalidates queued dispatch.
    agent_ui::Navigation::Get().Open(agent_ui::ScreenId::Home);
    auto stale = invoke("text"); ui_test::RunPosts(); ui_test::Tick();
    auto token = Availability::Get().AcquireBlock("test"); Availability::Get().ReleaseBlock(token);
    const int before = sent; ui_test::Tick();
    assert(sent == before && status(stale) == OperationStatus::Failed);
    for (int i = 0; i < 80; ++i) {
        auto item = invoke("text"); ui_test::RunPosts(); ui_test::Tick();
        ack("codex_text_result"); assert(status(item) == OperationStatus::Succeeded);
    }
    std::cout << "Codex production provider: navigation, identity/type matching, realtime leases, failure, cancellation and bounded receipts passed\n";
}
