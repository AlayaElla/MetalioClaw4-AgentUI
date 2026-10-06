#pragma once

#include <cstdint>

namespace display_power {

enum class TransportCreateResult : uint8_t {
    Ready,
    StaleIoDeleteFailed,
    StaleBusDeleteFailed,
    BusCreateFailed,
    IoCreateFailed,
};

enum class TransportDeleteResult : uint8_t {
    Deleted,
    IoDeleteFailed,
    BusDeleteFailed,
    PowerReleaseFailed,
};

// Ops owns the real handles. Failed deletes must leave their corresponding
// handle stored so a later wake attempt can resume cleanup safely.
template <typename Ops>
TransportDeleteResult DestroyPanelTransport(Ops& ops) {
    ops.unpublish_io();
    if (!ops.delete_io()) return TransportDeleteResult::IoDeleteFailed;
    if (!ops.delete_bus()) return TransportDeleteResult::BusDeleteFailed;
    // Never remove PHY power while a live bus can still access it.
    if (!ops.release_phy_power()) return TransportDeleteResult::PowerReleaseFailed;
    return TransportDeleteResult::Deleted;
}

// Reset stale, partially-created resources before starting a fresh host. This
// makes retries safe when bus creation succeeds but IO/panel creation fails.
template <typename Ops>
TransportCreateResult CreatePanelTransport(Ops& ops) {
    ops.unpublish_io();
    if (!ops.delete_stale_io()) return TransportCreateResult::StaleIoDeleteFailed;
    if (!ops.delete_stale_bus()) return TransportCreateResult::StaleBusDeleteFailed;
    if (!ops.create_bus()) return TransportCreateResult::BusCreateFailed;
    if (!ops.create_io()) return TransportCreateResult::IoCreateFailed;
    return TransportCreateResult::Ready;
}

enum class PanelWakeResult : uint8_t {
    Recovered,
    Dma2dFailed,
    PanelWakeFailed,
    AdapterRecoveryFailed,
    IoPublishFailed,
};

// This is the production order used before leaving the black recovery state.
// Adapter recovery receives the new IO handle held by Ops, never a stale one.
template <typename Ops>
PanelWakeResult RunPanelWakeTransaction(Ops& ops) {
    if (!ops.ensure_dma2d()) return PanelWakeResult::Dma2dFailed;
    if (!ops.wake_panel()) return PanelWakeResult::PanelWakeFailed;
    if (!ops.recover_adapter(ops.panel_io())) return PanelWakeResult::AdapterRecoveryFailed;
    if (!ops.publish_io()) return PanelWakeResult::IoPublishFailed;
    return PanelWakeResult::Recovered;
}

}  // namespace display_power
