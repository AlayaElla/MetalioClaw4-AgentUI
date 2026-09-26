#pragma once

namespace ai::system_connectivity {

// Static providers are discoverable at boot; invocation marshals the actual
// settings/phone controller work onto the LVGL thread.
void RegisterProviders();
void UnregisterProviders();

}  // namespace ai::system_connectivity
