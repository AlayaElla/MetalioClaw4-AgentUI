#pragma once
#include <string_view>

namespace agent_ui::external_apps {
// The recording service emits sanitized base names in this one directory.
inline bool IsRecordingWavPath(std::string_view path) {
    constexpr std::string_view root = "/sdcard/Recordings/";
    if (path.size() <= root.size() + 4 || path.size() >= 160 ||
        path.substr(0, root.size()) != root ||
        path.substr(path.size() - 4) != ".wav") return false;
    for (char c : path.substr(root.size(), path.size() - root.size() - 4)) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}
}  // namespace agent_ui::external_apps
