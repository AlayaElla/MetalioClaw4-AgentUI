#include "xiaozhi_server_settings.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <nvs.h>

#include "ai_settings.h"

namespace xiaozhi_server_settings {
namespace {

std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string ReadString(const char* ns, const char* key, bool* found = nullptr) {
    if (found) *found = false;
    nvs_handle_t handle = 0;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) return {};
    size_t size = 0;
    std::string value;
    if (nvs_get_str(handle, key, nullptr, &size) == ESP_OK && size > 0 &&
        size <= kMaxUrlLength + 1) {
        value.resize(size);
        if (nvs_get_str(handle, key, value.data(), &size) == ESP_OK) {
            value.resize(size - 1);
            if (found) *found = true;
        } else {
            value.clear();
        }
    }
    nvs_close(handle);
    return value;
}

bool WriteString(const char* key, const std::string& value) {
    nvs_handle_t handle = 0;
    if (nvs_open(ai_settings::kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_str(handle, key, value.c_str()) == ESP_OK &&
                    nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool ClearNamespace(const char* ns) {
    nvs_handle_t handle = 0;
    auto result = nvs_open(ns, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    nvs_close(handle);
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool ok = nvs_erase_all(handle) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

}  // namespace

Config Load(const std::string& build_default) {
    Config config;
    config.endpoint = ReadString(ai_settings::kNamespace, "server_url");
    if (config.endpoint.empty()) {
        config.endpoint = ReadString("wifi", "provisioning_url");
    }
    if (config.endpoint.empty()) config.endpoint = build_default;
    bool has_draft = false;
    config.custom_url = ReadString(ai_settings::kNamespace, "custom_url", &has_draft);
    if (!has_draft && config.IsCustom()) {
        config.custom_url = config.endpoint;
    }
    return config;
}

bool NormalizeUrl(const std::string& input, std::string& normalized) {
    normalized.clear();
    std::string url = Trim(input);
    if (url.empty() || url.size() > kMaxUrlLength) return false;
    for (unsigned char ch : url) {
        if (ch <= 0x20 || ch >= 0x7f || ch == '\\') return false;
    }
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    std::transform(url.begin(), url.begin() + scheme_end, url.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    if (url.compare(0, scheme_end, "http") != 0 &&
        url.compare(0, scheme_end, "https") != 0) return false;
    // Queries and fragments cannot be reused for the /activate request.
    if (url.find_first_of("?#@") != std::string::npos) return false;
    const auto start = scheme_end + 3;
    const auto path = url.find('/', start);
    const auto authority = url.substr(start, path == std::string::npos
                                                ? std::string::npos : path - start);
    if (authority.empty()) return false;
    size_t port_start = std::string::npos;
    if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string::npos || end <= 1 ||
            authority.substr(1, end - 1).find(':') == std::string::npos) return false;
        for (size_t i = 1; i < end; ++i) {
            if (!std::isxdigit(static_cast<unsigned char>(authority[i])) &&
                authority[i] != ':' && authority[i] != '.') return false;
        }
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') return false;
            port_start = end + 2;
        }
    } else {
        const auto colon = authority.find(':');
        const auto host = authority.substr(0, colon);
        if (host.empty() || host.front() == '.' || host.front() == '-' ||
            host.back() == '-' || host.find("..") != std::string::npos) return false;
        for (unsigned char ch : host) {
            if (!std::isalnum(ch) && ch != '-' && ch != '.') return false;
        }
        if (colon != std::string::npos) port_start = colon + 1;
    }
    if (port_start != std::string::npos) {
        const auto port = authority.substr(port_start);
        if (port.empty() || port.size() > 5) return false;
        unsigned value = 0;
        for (unsigned char ch : port) {
            if (!std::isdigit(ch)) return false;
            value = value * 10 + ch - '0';
        }
        if (value == 0 || value > 65535) return false;
    }
    if (path == std::string::npos || path == url.size() - 1) {
        if (path != std::string::npos) url.pop_back();
        url += "/xiaozhi/ota/";
    }
    if (url.size() > kMaxUrlLength) return false;
    normalized = std::move(url);
    return true;
}

bool SaveDraft(const std::string& input) {
    const auto value = Trim(input);
    return value.size() <= kMaxUrlLength && WriteString("custom_url", value);
}

bool Apply(bool custom, const std::string& input) {
    std::string endpoint = kOfficialUrl;
    if (custom && (!NormalizeUrl(input, endpoint) || !SaveDraft(endpoint))) {
        return false;
    }
    // A single active endpoint key prevents a mixed mode/address on power loss.
    return WriteString("server_url", endpoint);
}

bool PrepareConnectionCache(const std::string& endpoint) {
    if (ReadString(ai_settings::kNamespace, "config_source") == endpoint) return true;
    return ClearNamespace("mqtt") && ClearNamespace("websocket") &&
           WriteString("config_source", endpoint);
}

}  // namespace xiaozhi_server_settings
