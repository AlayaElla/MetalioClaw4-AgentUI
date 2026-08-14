#pragma once

#include <string>
#include <vector>

namespace agent_ui::external_apps {

struct AppInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string root_path;
    std::string entry_path;
    std::string icon_path;
};

class Manager {
public:
    static Manager& Get();

    bool Refresh(std::string* error = nullptr);
    const std::vector<AppInfo>& apps() const { return apps_; }

    bool Select(const std::string& id);
    const AppInfo* selected_app() const;

    static constexpr const char* kPackagesRoot = "/sdcard/metalio/apps";
    static constexpr const char* kInstalledRoot = "/sdcard/metalio/.installed_apps";

private:
    Manager() = default;

    std::vector<AppInfo> apps_;
    std::string selected_id_;
};

}  // namespace agent_ui::external_apps
