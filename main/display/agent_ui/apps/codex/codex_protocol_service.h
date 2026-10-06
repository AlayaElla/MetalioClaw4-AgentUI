#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

struct CodexWsMessage;

namespace agent_ui::codex_protocol {

// Protocol-facing state and command admission for the Codex page. It has no
// LVGL dependency; UI delivery is an injected event sink.
class Service {
public:
    struct TransportContext {
        uint32_t app_generation = 0;
        uint32_t connection_generation = 0;
        uint32_t connection_epoch = 0;
        bool valid = false;
    };

    enum class Admission : uint8_t { Accepted, Rejected };
    enum class EventKind : uint8_t { Message, Status, Discovery, SendFailure };

    struct SendResult {
        bool sent = false;
        TransportContext context;
    };

    struct Event {
        EventKind kind = EventKind::Status;
        uint64_t activation_generation = 0;
        TransportContext context{};
        bool connected = false;
        std::shared_ptr<const CodexWsMessage> message;
        std::string name;
        std::string ip;
        int port = 0;
        std::string ai_request_id;
        std::string failure_message;
        std::function<void()> on_failure;
    };

    class Port {
    public:
        using Completion = std::function<void(const SendResult&)>;
        using MessageCallback = std::function<bool(std::shared_ptr<const CodexWsMessage>)>;
        using StatusCallback = std::function<void(bool, uint32_t, uint32_t, uint32_t)>;
        using DiscoveryCallback = std::function<void(const std::string&, const std::string&, int)>;
        virtual ~Port() = default;

        virtual void Init() = 0;
        virtual void SetCallbacks(MessageCallback message, StatusCallback status,
                                  DiscoveryCallback discovery) = 0;
        virtual void SetAppActive(bool active) = 0;
        virtual TransportContext CaptureContext() const = 0;
        virtual bool IsConnected() const = 0;
        virtual bool IsContextCurrent(const TransportContext& context,
                                      bool require_connected) const = 0;
        virtual Admission QueueTextMessage(std::string payload,
                                           const TransportContext& context,
                                           Completion completion) = 0;
        virtual bool Connect(const std::string& address) = 0;
        virtual bool Connect(const std::string& ip, int port) = 0;
        virtual bool RequestConnectAsync(const std::string& address) = 0;
        virtual bool RequestReconnectAsync() = 0;
        virtual bool RequestReconnectAsync(const TransportContext& context) = 0;
        virtual void Disconnect() = 0;
        virtual bool StartDiscovery(int timeout_ms) = 0;
        virtual void SetDiscoveryEnabled(bool enabled) = 0;
        virtual bool HasToken() const = 0;
        virtual bool LoadToken(std::string& token) const = 0;
        virtual bool SaveToken(const std::string& token) = 0;
        virtual std::string CurrentIp() const = 0;
        virtual int CurrentPort() const = 0;
        virtual void FailPendingRequest(const std::string& request_id,
                                        const std::string& message) = 0;
    };

    using EventSink = std::function<bool(Event)>;

    explicit Service(Port& port) : port_(port) {}
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    void Activate(EventSink sink);
    void Deactivate();
    bool IsActive() const;
    uint64_t ActivationGeneration() const;
    bool IsActivationCurrent(uint64_t generation) const;
    bool IsEventCurrent(uint64_t generation, const TransportContext& context,
                        bool require_connected) const;
    bool AcknowledgeContext(uint64_t generation, const TransportContext& context);
    TransportContext CaptureSendContext() const;
    bool IsConnected() const;
    bool IsContextCurrent(const TransportContext& context,
                         bool require_connected = true) const;

    Admission QueueCommand(std::string json, std::string ai_request_id = {},
                           std::string failure_message = "发送失败，请检查连接后重试",
                           std::function<void()> on_failure = {},
                           // This transport-thread callback must be thread-safe
                           // and must never touch LVGL. It runs only after a
                           // complete local WebSocket write succeeds.
                           std::function<void()> on_sent = {},
                           const TransportContext* captured_context = nullptr);
    void PostSendFailure(const TransportContext& context, std::string message,
                         std::function<void()> on_failure = {});

    bool Connect(const std::string& address);
    bool Connect(const std::string& ip, int port);
    bool RequestConnectAsync(const std::string& address);
    bool RequestReconnectAsync();
    bool RequestReconnectAsync(const TransportContext& context);
    void Disconnect();
    bool StartDiscovery(int timeout_ms = 5000);
    void SetDiscoveryEnabled(bool enabled);
    bool HasToken() const;
    bool LoadToken(std::string& token) const;
    bool SaveToken(const std::string& token);
    std::string CurrentIp() const;
    int CurrentPort() const;

private:
    bool OnMessage(uint64_t registration_generation,
                   std::shared_ptr<const CodexWsMessage> message);
    void OnStatus(uint64_t registration_generation, bool connected, uint32_t app_generation,
                  uint32_t connection_generation, uint32_t connection_epoch);
    void OnDiscovery(uint64_t registration_generation,
                     const std::string& name, const std::string& ip, int port);
    bool ObserveContext(const TransportContext& context, bool connected,
                        uint64_t registration_generation,
                        uint64_t* activation_generation);
    bool Publish(Event event);
    static bool SameTransport(const TransportContext& left,
                              const TransportContext& right);

    Port& port_;
    mutable std::mutex mutex_;
    bool active_ = false;
    bool context_initialized_ = false;
    bool context_change_pending_ = false;
    TransportContext last_context_{};
    uint64_t activation_generation_ = 0;
    EventSink event_sink_;
};

// Process-lifetime owner used by the Codex page. The transport adapter and
// this service outlive any page activation and all in-flight send callbacks.
Service& GetService();

}  // namespace agent_ui::codex_protocol
