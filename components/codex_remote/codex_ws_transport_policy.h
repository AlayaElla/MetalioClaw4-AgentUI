#ifndef CODEX_WS_TRANSPORT_POLICY_H
#define CODEX_WS_TRANSPORT_POLICY_H

#include <atomic>
#include <cstdint>

namespace codex_remote::transport {

// ESP WebSocket client interprets the ping values as seconds and the remaining
// values as milliseconds. A silent peer is therefore detected within about ten
// seconds, before the UI's application-level heartbeat is the only signal.
constexpr int kPingIntervalSeconds = 5;
constexpr int kPongTimeoutSeconds = 5;
constexpr int kReconnectTimeoutMs = 1500;
constexpr int kNetworkTimeoutMs = 5000;
constexpr int kSilentDisconnectBoundMs =
    (kPingIntervalSeconds + kPongTimeoutSeconds) * 1000;

constexpr bool CanReconnect(bool has_endpoint, bool has_token) {
    return has_endpoint && has_token;
}

class ConnectionState {
public:
    bool MarkConnected() {
        const bool was_connected = connected_.exchange(true);
        if (!was_connected) {
            connection_generation_.fetch_add(1);
        }
        return !was_connected;
    }

    bool MarkDisconnected() {
        return connected_.exchange(false);
    }

    bool IsConnected() const { return connected_.load(); }
    uint32_t Generation() const { return connection_generation_.load(); }

private:
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> connection_generation_{0};
};

}  // namespace codex_remote::transport

#endif  // CODEX_WS_TRANSPORT_POLICY_H
