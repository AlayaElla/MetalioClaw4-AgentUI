#pragma once

#include <cstdint>
#include <cstring>

namespace agent_ui::codex_bridge {

// The WebSocket only proves that a peer still accepts frames.  The PC bridge
// sends bridge_status independently so the device can distinguish its own
// transport from the bridge's Codex connection.
enum class Status {
    TransportDisconnected,
    Synchronizing,
    Stopping,
    Unresponsive,
    CodexDisconnected,
    Ready,
};

class ConnectionState {
public:
    static constexpr uint32_t kSilenceTimeoutMs = 8000;

    void SetTransportConnected(bool connected, uint32_t now_ms) {
        if (transport_connected_ == connected) return;
        transport_connected_ = connected;
        seen_status_ = false;
        bridge_ready_ = false;
        bridge_stopping_ = false;
        codex_connected_ = false;
        transport_connected_at_ms_ = now_ms;
        last_status_ms_ = now_ms;
        sync_requested_ = false;
    }

    // Returns false for an incompatible or malformed status frame.  Such a
    // frame deliberately cannot keep a stale bridge alive.
    bool ApplyBridgeStatus(int version, const char* state, bool codex_connected,
                           uint32_t now_ms) {
        if (!transport_connected_ || version != 1 || state == nullptr) return false;
        if (std::strcmp(state, "ready") == 0) {
            bridge_ready_ = true;
            bridge_stopping_ = false;
        } else if (std::strcmp(state, "stopping") == 0) {
            bridge_ready_ = false;
            bridge_stopping_ = true;
        } else {
            return false;
        }
        seen_status_ = true;
        codex_connected_ = codex_connected;
        last_status_ms_ = now_ms;
        return true;
    }

    Status Current(uint32_t now_ms) const {
        if (!transport_connected_) return Status::TransportDisconnected;
        if (!seen_status_) {
            return static_cast<uint32_t>(now_ms - transport_connected_at_ms_) >
                           kSilenceTimeoutMs
                       ? Status::Unresponsive
                       : Status::Synchronizing;
        }
        if (static_cast<uint32_t>(now_ms - last_status_ms_) > kSilenceTimeoutMs) {
            return Status::Unresponsive;
        }
        if (bridge_stopping_) return Status::Stopping;
        if (!bridge_ready_) return Status::Synchronizing;
        return codex_connected_ ? Status::Ready : Status::CodexDisconnected;
    }

    bool CanUseCodex(uint32_t now_ms) const { return Current(now_ms) == Status::Ready; }

    // Ask for one fresh snapshot for each transition into a usable bridge.
    // Calling this while unavailable re-arms the next recovery without using
    // socket pings or old codex_state frames as a liveness signal.
    bool ConsumeSyncRequest(uint32_t now_ms) {
        if (!CanUseCodex(now_ms)) {
            sync_requested_ = false;
            return false;
        }
        if (sync_requested_) return false;
        sync_requested_ = true;
        return true;
    }

    void ResetSyncRequest() { sync_requested_ = false; }

private:
    bool transport_connected_ = false;
    bool seen_status_ = false;
    bool bridge_ready_ = false;
    bool bridge_stopping_ = false;
    bool codex_connected_ = false;
    bool sync_requested_ = false;
    uint32_t transport_connected_at_ms_ = 0;
    uint32_t last_status_ms_ = 0;
};

}  // namespace agent_ui::codex_bridge
