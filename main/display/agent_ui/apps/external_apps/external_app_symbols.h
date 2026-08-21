#pragma once

namespace agent_ui::external_apps {

// Registers the narrow native-symbol ABI used by validated external ELFs.
// Registration is process-global and idempotent for the firmware lifetime.
bool EnsureExternalAppSymbolsRegistered();

}  // namespace agent_ui::external_apps
