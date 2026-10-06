#include "codex_protocol_service.h"

#include <utility>

#include "codex_ws_client.h"

namespace agent_ui::codex_protocol {


bool Service::SameTransport(const TransportContext& left,
                            const TransportContext& right) {
    return left.valid == right.valid &&
        left.app_generation == right.app_generation &&
        left.connection_generation == right.connection_generation &&
        left.connection_epoch == right.connection_epoch;
}

void Service::Activate(EventSink sink) {
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = true;
        generation = ++activation_generation_;
        context_initialized_ = false;
        context_change_pending_ = false;
        last_context_ = {};
        event_sink_ = std::move(sink);
    }

    port_.Init();
    port_.SetCallbacks(
        [this, generation](std::shared_ptr<const CodexWsMessage> message) {
            // The transport retries a message callback when it returns false.
            // A registration already invalidated by page reactivation has
            // intentionally discarded this message, so acknowledge it here.
            if (!IsActivationCurrent(generation)) return true;
            return OnMessage(generation, std::move(message));
        },
        [this, generation](bool connected, uint32_t app_generation,
               uint32_t connection_generation, uint32_t connection_epoch) {
            if (!IsActivationCurrent(generation)) return;
            OnStatus(generation, connected, app_generation, connection_generation,
                     connection_epoch);
        },
        [this, generation](const std::string& name, const std::string& ip, int port) {
            if (!IsActivationCurrent(generation)) return;
            OnDiscovery(generation, name, ip, port);
        });
    port_.SetAppActive(true);

}

void Service::Deactivate() {
    bool was_active;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_active = active_;
        active_ = false;
        ++activation_generation_;
        event_sink_ = {};
        context_initialized_ = false;
        context_change_pending_ = false;
        last_context_ = {};
    }
    if (!was_active) return;
    port_.SetCallbacks({}, {}, {});
    port_.SetAppActive(false);
}

bool Service::IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

uint64_t Service::ActivationGeneration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activation_generation_;
}

bool Service::IsActivationCurrent(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && generation == activation_generation_;
}

bool Service::IsEventCurrent(uint64_t generation, const TransportContext& context,
                             bool require_connected) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || generation != activation_generation_) return false;
    }
    return !context.valid || port_.IsContextCurrent(context, require_connected);
}

bool Service::AcknowledgeContext(uint64_t generation,
                                 const TransportContext& context) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || generation != activation_generation_ ||
        !context_initialized_ || !SameTransport(last_context_, context)) return false;
    const bool changed = context_change_pending_;
    context_change_pending_ = false;
    return changed;
}

Service::TransportContext Service::CaptureSendContext() const {
    return port_.CaptureContext();
}

bool Service::IsConnected() const { return port_.IsConnected(); }

bool Service::IsContextCurrent(const TransportContext& context,
                               bool require_connected) const {
    return port_.IsContextCurrent(context, require_connected);
}

Service::Admission Service::QueueCommand(
    std::string json, std::string ai_request_id, std::string failure_message,
    std::function<void()> on_failure, std::function<void()> on_sent,
    const TransportContext* captured_context) {
    const TransportContext context = captured_context == nullptr
        ? port_.CaptureContext() : *captured_context;
    uint64_t activation_generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) return Admission::Rejected;
        activation_generation = activation_generation_;
    }
    if (!context.valid || !port_.IsContextCurrent(context, true)) return Admission::Rejected;

    return port_.QueueTextMessage(std::move(json), context,
        [this, context, activation_generation,
         ai_request_id = std::move(ai_request_id),
         failure_message = std::move(failure_message),
         on_failure = std::move(on_failure),
         on_sent = std::move(on_sent)](const SendResult& result) mutable {
            if (!SameTransport(result.context, context) ||
                !IsEventCurrent(activation_generation, context, false)) return;
            if (result.sent) {
                if (on_sent) on_sent();
                return;
            }
            if (!ai_request_id.empty())
                port_.FailPendingRequest(ai_request_id, failure_message);
            Event event;
            event.kind = EventKind::SendFailure;
            event.activation_generation = activation_generation;
            event.context = context;
            event.ai_request_id = std::move(ai_request_id);
            event.failure_message = std::move(failure_message);
            event.on_failure = std::move(on_failure);
            Publish(std::move(event));
        });
}

void Service::PostSendFailure(const TransportContext& context, std::string message,
                              std::function<void()> on_failure) {
    uint64_t activation_generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) return;
        activation_generation = activation_generation_;
    }
    if (!context.valid || !port_.IsContextCurrent(context, true)) return;
    Event event;
    event.kind = EventKind::SendFailure;
    event.activation_generation = activation_generation;
    event.context = context;
    event.failure_message = std::move(message);
    event.on_failure = std::move(on_failure);
    Publish(std::move(event));
}

bool Service::Connect(const std::string& address) { return port_.Connect(address); }
bool Service::Connect(const std::string& ip, int port) { return port_.Connect(ip, port); }
bool Service::RequestConnectAsync(const std::string& address) {
    return port_.RequestConnectAsync(address);
}
bool Service::RequestReconnectAsync() { return port_.RequestReconnectAsync(); }
bool Service::RequestReconnectAsync(const TransportContext& context) {
    return port_.RequestReconnectAsync(context);
}
void Service::Disconnect() { port_.Disconnect(); }
bool Service::StartDiscovery(int timeout_ms) { return port_.StartDiscovery(timeout_ms); }
void Service::SetDiscoveryEnabled(bool enabled) { port_.SetDiscoveryEnabled(enabled); }
bool Service::HasToken() const { return port_.HasToken(); }
bool Service::LoadToken(std::string& token) const { return port_.LoadToken(token); }
bool Service::SaveToken(const std::string& token) { return port_.SaveToken(token); }
std::string Service::CurrentIp() const { return port_.CurrentIp(); }
int Service::CurrentPort() const { return port_.CurrentPort(); }

bool Service::ObserveContext(const TransportContext& context, bool connected,
                             uint64_t registration_generation,
                             uint64_t* activation_generation) {
    if (!context.valid || !port_.IsContextCurrent(context, connected)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || registration_generation != activation_generation_) return false;
    if (!context_initialized_ || !SameTransport(last_context_, context)) {
        context_initialized_ = true;
        context_change_pending_ = true;
        last_context_ = context;
    }
    *activation_generation = activation_generation_;
    return true;
}

bool Service::Publish(Event event) {
    EventSink sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || event.activation_generation != activation_generation_) return false;
        sink = event_sink_;
    }
    return sink ? sink(std::move(event)) : false;
}

bool Service::OnMessage(uint64_t registration_generation,
                        std::shared_ptr<const CodexWsMessage> message) {
    if (!message) return false;
    Event event;
    event.kind = EventKind::Message;
    event.context = {message->app_generation, message->connection_generation,
                     message->connection_epoch, true};
    if (!ObserveContext(event.context, true, registration_generation,
                        &event.activation_generation)) return true;
    event.message = std::move(message);
    return Publish(std::move(event));
}

void Service::OnStatus(uint64_t registration_generation, bool connected,
                       uint32_t app_generation,
                       uint32_t connection_generation, uint32_t connection_epoch) {
    Event event;
    event.kind = EventKind::Status;
    event.connected = connected;
    event.context = {app_generation, connection_generation, connection_epoch, true};
    if (!ObserveContext(event.context, connected, registration_generation,
                        &event.activation_generation)) return;
    Publish(std::move(event));
}

void Service::OnDiscovery(uint64_t registration_generation,
                          const std::string& name, const std::string& ip, int port) {
    Event event;
    event.kind = EventKind::Discovery;
    event.name = name;
    event.ip = ip;
    event.port = port;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || registration_generation != activation_generation_) return;
        event.activation_generation = registration_generation;
    }
    event.context = port_.CaptureContext();
    Publish(std::move(event));
}

}  // namespace agent_ui::codex_protocol
