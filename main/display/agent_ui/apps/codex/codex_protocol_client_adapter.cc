#include "codex_protocol_service.h"

#include <utility>

#include "codex_ai_provider.h"
#include "codex_ws_client.h"

namespace agent_ui::codex_protocol {
namespace {

Service::TransportContext ToServiceContext(const CodexWsClient::SendContext& context) {
    return {context.app_generation, context.connection_generation,
            context.connection_epoch, context.valid};
}

CodexWsClient::SendContext ToClientContext(const Service::TransportContext& context) {
    return {context.app_generation, context.connection_generation,
            context.connection_epoch, context.valid};
}

class CodexClientPort final : public Service::Port {
public:
    void Init() override { CodexWsClient::GetInstance().Init(); }

    void SetCallbacks(MessageCallback message, StatusCallback status,
                      DiscoveryCallback discovery) override {
        auto& client = CodexWsClient::GetInstance();
        client.SetOnMessageCallback(std::move(message));
        client.SetOnStatusCallback(std::move(status));
        client.SetOnDiscoveryCallback(std::move(discovery));
    }

    void SetAppActive(bool active) override {
        CodexWsClient::GetInstance().SetAppActive(active);
    }

    Service::TransportContext CaptureContext() const override {
        return ToServiceContext(CodexWsClient::GetInstance().CaptureSendContext());
    }

    bool IsConnected() const override {
        return CodexWsClient::GetInstance().IsConnected();
    }

    bool IsContextCurrent(const Service::TransportContext& context,
                          bool require_connected) const override {
        const auto& client = CodexWsClient::GetInstance();
        return context.valid && client.IsAppActive() &&
            client.GetAppSessionGeneration() == context.app_generation &&
            client.GetConnectionGeneration() == context.connection_generation &&
            client.GetConnectionEpoch() == context.connection_epoch &&
            (!require_connected || client.IsConnected());
    }

    Service::Admission QueueTextMessage(std::string payload,
                                        const Service::TransportContext& context,
                                        Completion completion) override {
        const auto admission = CodexWsClient::GetInstance().QueueTextMessage(
            std::move(payload), ToClientContext(context),
            [completion = std::move(completion)](
                const CodexWsClient::SendResult& result) mutable {
                completion({result.sent,
                    {result.app_generation, result.connection_generation,
                     result.connection_epoch, true}});
            });
        return admission == CodexWsClient::SendAdmission::Accepted
            ? Service::Admission::Accepted : Service::Admission::Rejected;
    }

    bool Connect(const std::string& address) override {
        return CodexWsClient::GetInstance().Connect(address);
    }

    bool Connect(const std::string& ip, int port) override {
        return CodexWsClient::GetInstance().Connect(ip, port);
    }

    bool RequestConnectAsync(const std::string& address) override {
        return CodexWsClient::GetInstance().RequestConnectAsync(address);
    }

    bool RequestReconnectAsync() override {
        return CodexWsClient::GetInstance().RequestReconnectAsync();
    }

    bool RequestReconnectAsync(const Service::TransportContext& context) override {
        return CodexWsClient::GetInstance().RequestReconnectAsync(ToClientContext(context));
    }

    void Disconnect() override { CodexWsClient::GetInstance().Disconnect(); }

    bool StartDiscovery(int timeout_ms) override {
        return CodexWsClient::GetInstance().StartDiscovery(timeout_ms);
    }

    void SetDiscoveryEnabled(bool enabled) override {
        CodexWsClient::GetInstance().SetDiscoveryEnabled(enabled);
    }

    bool HasToken() const override { return CodexWsClient::GetInstance().HasToken(); }

    bool LoadToken(std::string& token) const override {
        return CodexWsClient::GetInstance().LoadToken(token);
    }

    bool SaveToken(const std::string& token) override {
        return CodexWsClient::GetInstance().SaveToken(token);
    }

    std::string CurrentIp() const override {
        return CodexWsClient::GetInstance().GetCurrentIp();
    }

    int CurrentPort() const override {
        return CodexWsClient::GetInstance().GetCurrentPort();
    }

    void FailPendingRequest(const std::string& request_id,
                            const std::string& message) override {
        codex_ai::FailPendingRequest(request_id, message);
    }
};

}  // namespace

Service& GetService() {
    // Both objects outlive every page activation and queued TX callback.
    static CodexClientPort port;
    static Service service(port);
    return service;
}

}  // namespace agent_ui::codex_protocol
