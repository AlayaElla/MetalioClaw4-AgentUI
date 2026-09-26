#include "app_mcp_tools.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <functional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <cJSON.h>

#include "ai/ai_capabilities.h"
#include "ai/ai_availability.h"
#include "ai/ai_ui_operation.h"
#include "application.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "agent_ui_types.h"
#include "apps/files/files_view.h"
#include "apps/standby/standby_view.h"
#include "apps/external_apps/external_app_manager.h"
#include "SdCardManager.hpp"
#include "usb_virtual_disk.h"
#include "core/idle_power.h"
#include "core/navigation.h"
#include "core/theme.h"
#include "mcp_server.h"

namespace agent_ui {
namespace {
struct Route { const char* id; ScreenId screen; };
constexpr std::array<Route, 5> kRoutes = {{{"codex", ScreenId::Codex}, {"camera", ScreenId::Camera}, {"phone", ScreenId::Phone}, {"files", ScreenId::Files}, {"settings", ScreenId::Settings}}};

ai::CapabilityState NavigationState() {
    ai::CapabilityState state;
    state.state_json = "{\"apps\":[\"codex\",\"camera\",\"phone\",\"files\",\"settings\"]}";
    return state;
}
ai::CapabilityState DisplayState() {
    ai::CapabilityState state;
    state.state_json = std::string("{\"standbyMinutesAllowed\":[1,5,10,15,30],\"wakePreference\":") +
        (Application::GetInstance().IsAiWakeEnabled() ? "true}" : "false}");
    return state;
}
ai::CapabilityState FilesState() {
    ai::CapabilityState state;
    auto& sd = SdCardManager::GetInstance();
    auto& disk = UsbVirtualDisk::GetInstance();
    state.available = sd.IsMounted() && !disk.IsSdExportedToHost() && !disk.IsBusy();
    state.reason = state.available ? "" : "SD storage is unavailable or owned by USB";
    state.state_json = std::string("{\"mounted\":") + (sd.IsMounted() ? "true" : "false") +
        ",\"usbExported\":" + (disk.IsSdExportedToHost() ? "true" : "false") +
        ",\"usbBusy\":" + (disk.IsBusy() ? "true" : "false") + "}";
    return state;
}
ai::OperationResult Done(const std::string& json = "{}") { ai::OperationResult r; r.status = ai::OperationStatus::Succeeded; r.result_json = json; return r; }
ai::OperationResult Error(const std::string& text) { ai::OperationResult r; r.status = ai::OperationStatus::Failed; r.error = text; return r; }
bool Parse(const ai::InvokeRequest& request, cJSON** args) { *args = cJSON_Parse(request.arguments_json.c_str()); return *args != nullptr && cJSON_IsObject(*args); }
std::string String(const cJSON* args, const char* key) { const cJSON* value = cJSON_GetObjectItemCaseSensitive(args, key); return cJSON_IsString(value) && value->valuestring ? value->valuestring : ""; }
int Integer(const cJSON* args, const char* key) { const cJSON* value = cJSON_GetObjectItemCaseSensitive(args, key); return cJSON_IsNumber(value) && std::floor(value->valuedouble) == value->valuedouble ? value->valueint : -1; }
bool Boolean(const cJSON* args, const char* key, bool fallback = false) { const cJSON* value = cJSON_GetObjectItemCaseSensitive(args, key); return cJSON_IsBool(value) ? cJSON_IsTrue(value) : fallback; }

ai::OperationResult InvokeNavigation(const ai::InvokeRequest& request) {
    cJSON* args = nullptr; if (!Parse(request, &args)) return Error("invalid arguments");
    const std::string action = String(args, "action"), app = String(args, "app"); cJSON_Delete(args);
    return ai::UiOperations::Submit(request, [action, app] {
        if (action == "list") {
            cJSON* root = cJSON_CreateObject();
            auto* apps = cJSON_AddArrayToObject(root, "apps");
            for (const auto& route : kRoutes) cJSON_AddItemToArray(apps, cJSON_CreateString(route.id));
            for (const auto& installed : external_apps::Manager::Get().apps()) {
                auto* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "id", installed.id.c_str());
                cJSON_AddStringToObject(item, "name", installed.name.c_str());
                cJSON_AddItemToArray(apps, item);
            }
            char* encoded = cJSON_PrintUnformatted(root);
            auto result = Done(encoded ? encoded : "{}");
            cJSON_free(encoded); cJSON_Delete(root); return result;
        }
        if (action == "home") { Navigation::Get().Open(ScreenId::Home); return Done("{\"screen\":\"home\"}"); }
        if (action == "back") { Navigation::Get().Back(); return Done("{\"action\":\"back\"}"); }
        if (action != "open") return Error("action must be home, back, or open");
        if (StandbyView::IsActive()) return Error("device is in standby");
        for (const auto& route : kRoutes) if (app == route.id) { Navigation::Get().Open(route.screen); return Done("{\"app\":\"" + app + "\"}"); }
        if (external_apps::Manager::Get().Select(app)) {
            if (Navigation::Get().current() == ScreenId::ExternalAppHost) Navigation::Get().RebuildCurrent();
            else Navigation::Get().Open(ScreenId::ExternalAppHost);
            return Done("{\"hostOpened\":true,\"appStartupConfirmed\":false}");
        }
        return Error("app is not installed");
    });
}

ai::OperationResult InvokeDisplay(const ai::InvokeRequest& request) {
    cJSON* args = nullptr; if (!Parse(request, &args)) return Error("invalid arguments");
    if (String(args, "action") == "wake_enabled" && !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(args, "enabled"))) {
        cJSON_Delete(args); return Error("wake_enabled requires enabled: true or false");
    }
    const std::string action = String(args, "action"), text = String(args, "value"); const int value = Integer(args, "value"); const bool enabled = Boolean(args, "enabled"); cJSON_Delete(args);
    return ai::UiOperations::Submit(request, [action, text, value, enabled] {
        if (action == "volume") { auto* codec = Board::GetInstance().GetAudioCodec(); if (!codec || value < 0 || value > 100) return Error("volume unavailable or out of range"); codec->SetOutputVolume(value); return Done("{\"volume\":" + std::to_string(value) + "}"); }
        if (action == "brightness") { auto* light = Board::GetInstance().GetBacklight(); if (!light || value < 0 || value > 100) return Error("brightness unavailable or out of range"); light->SetBrightness(static_cast<uint8_t>(value), true); return Done("{\"brightness\":" + std::to_string(value) + "}"); }
        if (action == "theme") { if (text == "light") Theme::Get().SetAppearanceMode(AppearanceMode::Light); else if (text == "dark") Theme::Get().SetAppearanceMode(AppearanceMode::Dark); else return Error("theme must be light or dark"); Navigation::Get().RebuildCurrent(); return Done("{\"theme\":\"" + text + "\"}"); }
        if (action == "accent") { if (value < 0 || value > 3) return Error("accent must be 0..3"); Theme::Get().SetAccentPreset(static_cast<AccentPreset>(value)); Navigation::Get().RebuildCurrent(); return Done("{\"accent\":" + std::to_string(value) + "}"); }
        if (action == "standby_minutes") { if (value != 1 && value != 5 && value != 10 && value != 15 && value != 30) return Error("standby_minutes must be 1, 5, 10, 15, or 30"); IdlePower::Get().SetStandbyMinutes(value); return Done("{\"standbyMinutes\":" + std::to_string(value) + "}"); }
        if (action == "wake_enabled") { Application::GetInstance().SetAiWakeEnabled(enabled); return Done(enabled ? "{\"wakeEnabled\":true}" : "{\"wakeEnabled\":false}"); }
        return Error("unsupported display action");
    });
}

bool SdPath(const std::string& path) { return path == "/sdcard" || (path.rfind("/sdcard/", 0) == 0 && path.find("..") == std::string::npos); }
ai::OperationResult InvokeFiles(const ai::InvokeRequest& request) {
    cJSON* args = nullptr; if (!Parse(request, &args)) return Error("invalid arguments"); const std::string action = String(args, "action"), path = String(args, "path"); cJSON_Delete(args);
    if (path.find('\\') != std::string::npos) return Error("invalid SD path");
    if (action != "storage" && !SdPath(path)) return Error("path must stay under /sdcard");
    auto& sd = SdCardManager::GetInstance();
    auto& disk = UsbVirtualDisk::GetInstance();
    if (!sd.IsMounted()) return Error("SD storage unavailable");
    if (disk.IsSdExportedToHost() || disk.IsBusy()) return Error("SD storage is in use by USB");
    if (action == "delete") {
        return ai::UiOperations::Submit(request, [path] {
            return FilesView::DeletePath(path.c_str()) ? Done("{\"deleted\":true}") : Error("delete failed or storage is busy");
        });
    }
    if (action == "preview") {
        return ai::UiOperations::Submit(request, [path, opened = false]() mutable {
            if (!opened) {
                Navigation::Get().Open(ScreenId::Files);
                opened = true;
                ai::OperationResult pending;
                pending.status = ai::OperationStatus::Pending;
                return pending;
            }
            return FilesView::PreviewPath(path.c_str()) ? Done("{\"previewing\":true}") : Error("file is not previewable or storage is busy");
        });
    }
    if (action == "read") { FILE* file = fopen(path.c_str(), "rb"); if (!file) return Error("file cannot be opened"); char buffer[1025] = {}; fread(buffer, 1, 1024, file); fclose(file); cJSON* o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "content", buffer); return {.status=ai::OperationStatus::Succeeded, .result_json=[](cJSON* x){char* p=cJSON_PrintUnformatted(x);std::string s=p?p:"{}";cJSON_free(p);cJSON_Delete(x);return s;}(o)}; }
    if (action == "list") { DIR* dir = opendir(path.c_str()); if (!dir) return Error("directory cannot be opened"); cJSON* o=cJSON_CreateObject(); cJSON* list=cJSON_AddArrayToObject(o,"entries"); for (dirent* e=readdir(dir); e && cJSON_GetArraySize(list)<64; e=readdir(dir)) if (std::string(e->d_name)!="." && std::string(e->d_name)!="..") cJSON_AddItemToArray(list,cJSON_CreateString(e->d_name)); closedir(dir); char* p=cJSON_PrintUnformatted(o); std::string json=p?p:"{}"; cJSON_free(p); cJSON_Delete(o); return Done(json); }
    if (action == "storage") {
        uint64_t total = 0, free = 0;
        if (!FilesView::GetStorageBytes(&total, &free)) return Error("SD storage unavailable");
        return Done("{\"mounted\":true,\"totalBytes\":" + std::to_string(total) +
                    ",\"freeBytes\":" + std::to_string(free) + "}");
    }
    return Error("action must be list, read, preview, delete, or storage");
}

std::string Render(const ai::OperationResult& result) { cJSON* o=cJSON_CreateObject(); cJSON_AddStringToObject(o,"operationId",result.operation_id.c_str()); cJSON_AddStringToObject(o,"status",ai::OperationStatusName(result.status)); cJSON_AddNumberToObject(o,"generation",static_cast<double>(result.generation)); if (!result.error.empty()) cJSON_AddStringToObject(o,"error",result.error.c_str()); cJSON* payload=cJSON_Parse(result.result_json.c_str()); if(payload)cJSON_AddItemToObject(o,"result",payload); char* p=cJSON_PrintUnformatted(o);std::string s=p?p:"{}";cJSON_free(p);cJSON_Delete(o);return s; }
void Add(const char* id, const char* title, const char* description, const char* schema,
         std::function<ai::CapabilityState()> state,
         std::function<ai::OperationResult(const ai::InvokeRequest&)> invoke) {
    ai::CapabilityRegistry::Get().Register(
        {{id, title, description, schema}, std::move(state), std::move(invoke),
         ai::UiOperations::GetResult, ai::UiOperations::Cancel});
}
}  // namespace

void RegisterAppMcpTools() {
    static bool registered = false; if (registered) return; registered = true;
    Add("system.navigation", "Navigation", "List or open built-in and installed apps, home, or back.",
        "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"home\",\"back\",\"open\"]},\"app\":{\"type\":\"string\",\"maxLength\":80}},\"required\":[\"action\"]}", NavigationState, InvokeNavigation);
    Add("system.display", "Display controls", "Control volume, brightness, theme, accent, standby timeout, and AI wake preference.",
        "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"volume\",\"brightness\",\"theme\",\"accent\",\"standby_minutes\",\"wake_enabled\"]},\"value\":{},\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"action\"]}", DisplayState, InvokeDisplay);
    Add("files.storage", "Files and storage", "List, read, delete, and inspect files under /sdcard.",
        "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"read\",\"preview\",\"delete\",\"storage\"]},\"path\":{\"type\":\"string\"}},\"required\":[\"action\"]}", FilesState, InvokeFiles);
    auto& server = McpServer::GetInstance();
    server.AddTool("self.camera.capture",
        "Take a photo now and show it for review. For requests such as 帮我拍照, "
        "call this tool directly without capabilities.list/describe or app.open. "
        "Execute before saying the photo was taken. If pending, use "
        "self.capabilities.result with operationId until succeeded. "
        "The photo is not saved until the user chooses save.",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            ai::InvokeRequest request;
            request.capability_id = "camera.control";
            request.arguments_json = "{\"action\":\"capture\"}";
            return Render(ai::CapabilityRegistry::Get().Invoke(request));
        });
    server.AddTool("self.app.open", "Open a built-in Agent UI app.",
        PropertyList({Property("app", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            ai::InvokeRequest request;
            request.capability_id = "system.navigation";
            request.arguments_json = "{\"action\":\"open\",\"app\":\"" +
                properties["app"].value<std::string>() + "\"}";
            const ai::OperationResult result = ai::CapabilityRegistry::Get().Invoke(request);
            if (result.status == ai::OperationStatus::Failed) throw std::runtime_error(result.error);
            return Render(result);
        });
    server.AddTool("self.ai.availability", "Read device AI availability and active block reasons.",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            const auto state = ai::Availability::Get().GetSnapshot();
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "available", state.available);
            cJSON_AddNumberToObject(result, "generation", static_cast<double>(state.generation));
            cJSON_AddBoolToObject(result, "wakeEnabled", Application::GetInstance().IsAiWakeEnabled());
            cJSON* reasons = cJSON_AddArrayToObject(result, "reasons");
            for (const auto& reason : state.reasons) cJSON_AddItemToArray(reasons, cJSON_CreateString(reason.c_str()));
            return result;
        });
    server.AddTool("self.capabilities.list", "List generic device capabilities.", PropertyList(), [](const PropertyList&) -> ReturnValue { cJSON* o=cJSON_CreateObject();cJSON* a=cJSON_AddArrayToObject(o,"capabilities");for(const auto& d:ai::CapabilityRegistry::Get().List()){cJSON* e=cJSON_CreateObject();cJSON_AddStringToObject(e,"id",d.id.c_str());cJSON_AddStringToObject(e,"title",d.title.c_str());cJSON_AddItemToArray(a,e);}return o; });
    server.AddTool("self.capabilities.describe", "Describe a generic capability and its current state.", PropertyList({Property("id",kPropertyTypeString)}), [](const PropertyList& p)->ReturnValue{ai::CapabilityDescriptor d;ai::CapabilityState s;std::string e;if(!ai::CapabilityRegistry::Get().Describe(p["id"].value<std::string>(),&d,&s,&e))throw std::runtime_error(e);cJSON* o=cJSON_CreateObject();cJSON_AddStringToObject(o,"id",d.id.c_str());cJSON_AddStringToObject(o,"title",d.title.c_str());cJSON_AddStringToObject(o,"description",d.description.c_str());cJSON_AddBoolToObject(o,"available",s.available);cJSON_AddBoolToObject(o,"enabled",s.enabled);cJSON_AddNumberToObject(o,"generation",static_cast<double>(s.generation));if(!s.reason.empty())cJSON_AddStringToObject(o,"reason",s.reason.c_str());cJSON* schema=cJSON_Parse(d.args_schema_json.c_str());if(schema)cJSON_AddItemToObject(o,"argsSchema",schema);else cJSON_AddStringToObject(o,"argsSchema",d.args_schema_json.c_str());cJSON* state=cJSON_Parse(s.state_json.c_str());if(state)cJSON_AddItemToObject(o,"state",state);else cJSON_AddStringToObject(o,"state",s.state_json.c_str());return o;});
    server.AddTool("self.capabilities.invoke", "Invoke a capability; args is a JSON object string.", PropertyList({Property("id",kPropertyTypeString),Property("args",kPropertyTypeString,std::string("{}")),Property("request_id",kPropertyTypeString,std::string("")),Property("generation",kPropertyTypeInteger,0)}), [](const PropertyList& p)->ReturnValue{ai::InvokeRequest r;r.capability_id=p["id"].value<std::string>();r.arguments_json=p["args"].value<std::string>();r.request_id=p["request_id"].value<std::string>();r.generation=static_cast<uint64_t>(p["generation"].value<int>());return Render(ai::CapabilityRegistry::Get().Invoke(r));});
    server.AddTool("self.capabilities.result", "Read a pending operation.", PropertyList({Property("operation_id",kPropertyTypeString)}), [](const PropertyList&p)->ReturnValue{return Render(ai::CapabilityRegistry::Get().GetResult(p["operation_id"].value<std::string>()));});
    server.AddTool("self.capabilities.cancel", "Cancel a pending operation.", PropertyList({Property("operation_id",kPropertyTypeString)}), [](const PropertyList&p)->ReturnValue{std::string e;if(!ai::CapabilityRegistry::Get().Cancel(p["operation_id"].value<std::string>(),&e))throw std::runtime_error(e);return std::string("{\"cancelled\":true}");});
}
}  // namespace agent_ui
