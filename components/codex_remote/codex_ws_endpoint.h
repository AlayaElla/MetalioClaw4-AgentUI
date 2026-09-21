#pragma once

#include <string>

namespace codex_remote {
// Manual endpoints always require an explicit port, including ws/wss URLs.
inline bool ParseEndpoint(const std::string& input, std::string& uri,
                          std::string& host, int& port) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    std::string value = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
    for (unsigned char c : value) if (c <= 32 || c == 127 || c == '\\') return false;
    if (value.find("://") == std::string::npos) value = "ws://" + value;
    size_t start = 0;
    if (value.compare(0, 5, "ws://") == 0) start = 5;
    else if (value.compare(0, 6, "wss://") == 0) start = 6;
    else return false;
    if (value.find('#') != std::string::npos) return false;
    const auto end = value.find_first_of("/?", start);
    const std::string authority = value.substr(start, end - start);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == authority.size()) return false;
    const std::string parsed_host = authority.substr(0, colon);
    if (parsed_host.front() == '[') {
        if (parsed_host.size() < 4 || parsed_host.back() != ']' ||
            parsed_host.find(':') == std::string::npos ||
            parsed_host.find(']') != parsed_host.size() - 1) return false;
    } else {
        for (unsigned char c : parsed_host) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
        }
    }
    int parsed_port = 0;
    for (size_t i = colon + 1; i < authority.size(); ++i) {
        const char c = authority[i];
        if (c < '0' || c > '9') return false;
        parsed_port = parsed_port * 10 + (c - '0');
        if (parsed_port > 65535) return false;
    }
    if (parsed_port == 0) return false;
    uri = value;
    host = parsed_host;
    port = parsed_port;
    return true;
}
}
