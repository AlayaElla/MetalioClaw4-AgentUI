#include "app_mcp_tools.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "agent_ui_types.h"
#include "apps/standby/standby_view.h"
#include "mcp_server.h"
#include "navigation.h"
#include "ui_dispatcher.h"

namespace agent_ui {
namespace {

struct AppRoute {
    const char* canonical_name;
    const char* chinese_name;
    ScreenId screen;
};

constexpr std::array<AppRoute, 5> kAppRoutes = {{
    {"codex", "代码助手", ScreenId::Codex},
    {"camera", "相机", ScreenId::Camera},
    {"phone", "电话", ScreenId::Phone},
    {"files", "文件", ScreenId::Files},
    {"settings", "设置", ScreenId::Settings},
}};

std::string NormalizeAppName(std::string name) {
    const auto is_ascii_space = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!name.empty() && is_ascii_space(name.front())) name.erase(0, 1);
    while (!name.empty() && is_ascii_space(name.back())) name.pop_back();
    for (char& ch : name) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return name;
}

const AppRoute* FindAppRoute(const std::string& requested_name) {
    const std::string name = NormalizeAppName(requested_name);
    for (const auto& route : kAppRoutes) {
        if (name == route.canonical_name || name == route.chinese_name) {
            return &route;
        }
    }
    return nullptr;
}

void OpenAppOnUiThread(ScreenId screen) {
    if (StandbyView::IsActive()) {
        // Black standby is intentionally side-key-only. Remote AI actions
        // must not wake or replace the preserved screen.
        return;
    }
    Navigation::Get().Open(screen);
}

}  // namespace

void RegisterAppMcpTools() {
    McpServer::GetInstance().AddTool(
        "self.app.open",
        "Open an app on this device when the user asks to open or switch to "
        "it. The app must be one of: codex (代码助手), camera (相机), "
        "phone (电话), files (文件), settings (设置).",
        PropertyList({Property("app", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            const auto requested = properties["app"].value<std::string>();
            const AppRoute* route = FindAppRoute(requested);
            if (route == nullptr) {
                throw std::runtime_error(
                    "Unsupported app. Use codex, camera, phone, files, or "
                    "settings.");
            }

            const ScreenId screen = route->screen;
            if (!UiDispatcher::Post(
                    [screen]() { OpenAppOnUiThread(screen); })) {
                throw std::runtime_error("Device UI is not ready");
            }
            return std::string("Opening ") + route->canonical_name;
        });
}

}  // namespace agent_ui
