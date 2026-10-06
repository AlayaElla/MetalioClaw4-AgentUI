#pragma once

#include <cstdint>

namespace display_power {

enum class PanelSleepResult : uint8_t {
    Slept,
    ControllerSleepFailed,
    AdapterPrepareFailed,
    Dma2dDisableFailed,
    PanelDeleteFailed,
    TransportDeleteFailed,
    RollbackFailed,
};

// Ops is the hardware boundary. Keeping ordering and rollback here lets host
// tests inject failures into the same transaction used by LVAdapterDisplay.
template <typename Ops>
PanelSleepResult RunPanelSleepTransaction(Ops& ops) {
    auto rollback = [&ops](PanelSleepResult cause) {
        // Wake the controller before sleep_recover resumes the LVGL worker.
        if (ops.wake_panel() && ops.recover_adapter()) {
            return cause;
        }
        ops.mark_recovery_required();
        return PanelSleepResult::RollbackFailed;
    };

    if (!ops.sleep_panel()) {
        if (!ops.wake_panel()) {
            ops.mark_recovery_required();
            return PanelSleepResult::RollbackFailed;
        }
        return PanelSleepResult::ControllerSleepFailed;
    }
    if (!ops.prepare_adapter()) {
        if (!ops.wake_panel() || !ops.recover_adapter()) {
            ops.mark_recovery_required();
            return PanelSleepResult::RollbackFailed;
        }
        return PanelSleepResult::AdapterPrepareFailed;
    }
    if (!ops.disable_dma2d()) {
        return rollback(PanelSleepResult::Dma2dDisableFailed);
    }
    if (!ops.delete_panel()) {
        if (!ops.restore_dma2d()) {
            ops.mark_recovery_required();
            return PanelSleepResult::RollbackFailed;
        }
        return rollback(PanelSleepResult::PanelDeleteFailed);
    }
    if (!ops.delete_transport()) {
        ops.mark_recovery_required();
        return PanelSleepResult::TransportDeleteFailed;
    }
    return PanelSleepResult::Slept;
}

}  // namespace display_power
