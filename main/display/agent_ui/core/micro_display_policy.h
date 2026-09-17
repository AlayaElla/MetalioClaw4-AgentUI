#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include "cJSON.h"

namespace agent_ui {

struct MicroDisplayConfig {
    bool valid = false;
    int brightness = 75;
    uint32_t auto_dim_ms = 0;
    std::string activity_key;
};

inline MicroDisplayConfig ParseMicroDisplay(const cJSON* root) {
    MicroDisplayConfig result;
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "connected"))) return result;
    const auto* config = cJSON_GetObjectItemCaseSensitive(root, "microDisplay");
    const auto* brightness = cJSON_GetObjectItemCaseSensitive(config, "brightnessPercent");
    const auto* timeout = cJSON_GetObjectItemCaseSensitive(config, "autoDimMs");
    const auto* activity = cJSON_GetObjectItemCaseSensitive(config, "activityKey");
    if (!cJSON_IsNumber(brightness) || brightness->valuedouble < 0 || brightness->valuedouble > 100 ||
        brightness->valuedouble != brightness->valueint || !cJSON_IsString(activity) ||
        std::char_traits<char>::length(activity->valuestring) > 4096) return result;
    if (!cJSON_IsNull(timeout)) {
        if (!cJSON_IsNumber(timeout) || timeout->valuedouble != timeout->valueint) return result;
        const int ms = timeout->valueint;
        if (ms != 30000 && ms != 60000 && ms != 180000 && ms != 600000 && ms != 1800000 && ms != 3600000) return result;
        result.auto_dim_ms = static_cast<uint32_t>(ms);
    }
    result.valid = true;
    result.brightness = std::clamp(brightness->valueint, 5, 100);
    result.activity_key = activity->valuestring;
    return result;
}

// A temporary screen override. Never persists Micro settings into device NVS.
class MicroDisplayPolicy {
public:
    bool Apply(const MicroDisplayConfig& config, uint32_t now) {
        const bool changed = config.valid != config_.valid || config.brightness != config_.brightness ||
            config.auto_dim_ms != config_.auto_dim_ms || config.activity_key != config_.activity_key;
        config_ = config;
        if (changed) Activity(now);
        return changed;
    }
    void Activity(uint32_t now) { last_activity_ = now; }
    bool active() const { return config_.valid; }
    int Brightness(uint32_t now) const {
        const bool dim = config_.auto_dim_ms > 0 && static_cast<uint32_t>(now - last_activity_) >= config_.auto_dim_ms;
        return dim ? std::min(config_.brightness, 15) : config_.brightness;
    }
private:
    MicroDisplayConfig config_;
    uint32_t last_activity_ = 0;
};

}  // namespace agent_ui
