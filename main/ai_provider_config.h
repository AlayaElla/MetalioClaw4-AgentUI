#pragma once

#include <cctype>
#include <string>
#include <string_view>

enum class AiProvider {
    Xiaozhi,
    Hermes,
};

struct AiProviderConfig {
    AiProvider provider = AiProvider::Xiaozhi;
    std::string hermes_dashboard_url;
    std::string hermes_username;
    // Stored in NVS and never logged or returned through a non-secret UI model.
    std::string hermes_password;
    std::string hermes_profile;
};

namespace ai_provider_config {

constexpr std::string_view kNamespace = "agent_ai";
constexpr std::string_view kProviderKey = "provider";
constexpr std::string_view kHermesDashboardUrlKey = "hermes_durl";
constexpr std::string_view kHermesUsernameKey = "hermes_user";
constexpr std::string_view kHermesPasswordKey = "hermes_pass";
constexpr std::string_view kHermesProfileKey = "hermes_profile";
constexpr std::string_view kDefaultHermesDashboardUrl = "http://192.168.50.149:9119";

inline bool HasControl(std::string_view value) {
    for (const unsigned char character : value) {
        if (std::iscntrl(character)) return true;
    }
    return false;
}

inline bool HasWhitespaceOrControl(std::string_view value) {
    for (const unsigned char character : value) {
        if (std::iscntrl(character) || std::isspace(character)) return true;
    }
    return false;
}

inline bool IsValidHermesBaseUrl(std::string_view url) {
    constexpr std::string_view kHttp = "http://";
    constexpr std::string_view kHttps = "https://";
    if (url.empty() || url.size() > 192 || HasWhitespaceOrControl(url)) return false;
    const bool is_http = url.compare(0, kHttp.size(), kHttp) == 0;
    const bool is_https = url.compare(0, kHttps.size(), kHttps) == 0;
    if ((!is_http && !is_https) || url.find('@') != std::string_view::npos ||
        url.find('?') != std::string_view::npos || url.find('#') != std::string_view::npos) {
        return false;
    }
    const size_t host_begin = is_https ? kHttps.size() : kHttp.size();
    const size_t host_end = url.find('/', host_begin);
    return host_end != host_begin && url.find(':', host_begin) != host_begin;
}

inline bool IsValidHermesUsername(std::string_view username) {
    return !username.empty() && username.size() <= 128 && !HasControl(username);
}

inline bool IsValidHermesPassword(std::string_view password) {
    return !password.empty() && password.size() <= 512 && !HasControl(password);
}

inline bool IsValidHermesProfile(std::string_view profile) {
    if (profile.empty() || profile.size() > 128 || HasWhitespaceOrControl(profile)) return false;
    for (const unsigned char character : profile) {
        if (!(std::isalnum(character) || character == '-' || character == '_' ||
              character == '.' || character == ':')) {
            return false;
        }
    }
    return true;
}

inline bool IsCompleteHermesConfig(const AiProviderConfig& config) {
    return IsValidHermesBaseUrl(config.hermes_dashboard_url) &&
        IsValidHermesUsername(config.hermes_username) &&
        IsValidHermesPassword(config.hermes_password) &&
        IsValidHermesProfile(config.hermes_profile);
}

inline std::string NormalizeHermesBaseUrl(std::string_view url) {
    std::string normalized(url);
    while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
    return normalized;
}

inline std::string ResolveHermesBaseUrl(std::string_view url) {
    const std::string normalized = NormalizeHermesBaseUrl(url);
    return normalized.empty() ? std::string(kDefaultHermesDashboardUrl) : normalized;
}

inline std::string UrlEncode(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) {
        if (std::isalnum(character) || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[(character >> 4) & 0x0f]);
            encoded.push_back(kHex[character & 0x0f]);
        }
    }
    return encoded;
}

inline std::string HermesProfilesUrl(std::string_view base_url) {
    return NormalizeHermesBaseUrl(base_url) + "/api/profiles";
}

AiProviderConfig Load();
void SaveHermesDraft(const AiProviderConfig& config);
bool Save(const AiProviderConfig& config);
const char* ToString(AiProvider provider);
AiProvider ParseProvider(std::string_view provider);

}  // namespace ai_provider_config
