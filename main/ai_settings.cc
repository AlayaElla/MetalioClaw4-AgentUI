#include "ai_settings.h"

#include <nvs.h>

namespace ai_settings {

bool MigrateLegacySettings() {
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    nvs_close(handle);

    result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return false;

    // These are persisted key names from firmware versions with direct
    // Hermes access. Do not erase the namespace: it also stores "wake".
    constexpr const char* obsolete_keys[] = {
        "provider", "hermes_durl", "hermes_user", "hermes_pass", "hermes_profile",
    };
    bool changed = false;
    for (const char* key : obsolete_keys) {
        result = nvs_erase_key(handle, key);
        if (result == ESP_OK) {
            changed = true;
        } else if (result != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            return false;
        }
    }
    result = changed ? nvs_commit(handle) : ESP_OK;
    nvs_close(handle);
    return result == ESP_OK;
}

}  // namespace ai_settings
