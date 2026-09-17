#pragma once

#include <cstddef>
#include <string>

namespace xiaozhi_server_settings {

inline constexpr char kOfficialUrl[] = "https://api.tenclass.net/xiaozhi/ota/";
inline constexpr size_t kMaxUrlLength = 512;

struct Config {
    std::string endpoint;
    std::string custom_url;
    bool IsCustom() const { return endpoint != kOfficialUrl; }
};

// Retain existing provisioning overrides until the user explicitly switches.
Config Load(const std::string& build_default);
bool NormalizeUrl(const std::string& input, std::string& normalized);
bool SaveDraft(const std::string& input);
// Changes the next boot's endpoint. An in-flight provisioning session is unchanged.
bool Apply(bool custom, const std::string& input);
// Run before provisioning, after reboot. Clear credentials only when their
// source changes; write the source marker after both namespace commits succeed.
bool PrepareConnectionCache(const std::string& endpoint);

}  // namespace xiaozhi_server_settings
