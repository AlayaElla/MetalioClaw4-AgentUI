#include "system_connectivity.h"

#include <cctype>
#include <cstring>
#include <functional>
#include <string>
#include <cmath>
#include <memory>
#include <cJSON.h>
#include "ai_availability.h"
#include "agent_ui/apps/network/network_module.h"
#include "agent_ui/apps/bluetooth/bluetooth_module.h"

#include "ai_capabilities.h"
#include "ai_ui_operation.h"
#include "agent_ui/apps/phone/phone_view.h"
#include "agent_ui/apps/settings/settings_view.h"
#include "agent_ui/apps/network/network_contract.h"
#include "agent_ui/apps/bluetooth/bluetooth_contract.h"
#include "agent_ui/core/navigation.h"

namespace ai::system_connectivity {
namespace {

OperationResult Fail(const char* error) { OperationResult r; r.status = OperationStatus::Failed; r.error = error; return r; }
OperationResult Pending() { OperationResult r; r.status = OperationStatus::Pending; return r; }
OperationResult Ok(const char* json) { OperationResult r; r.status = OperationStatus::Succeeded; r.result_json = json; return r; }

bool StringArg(const std::string& json, const char* key, std::string* out) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json.c_str()), cJSON_Delete);
    auto* value = cJSON_GetObjectItemCaseSensitive(root.get(), key);
    if (!cJSON_IsString(value) || !value->valuestring) return false;
    *out = value->valuestring;
    return out->size() <= 128;
}
bool IntArg(const std::string& json, const char* key, int* out) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json.c_str()), cJSON_Delete);
    auto* value = cJSON_GetObjectItemCaseSensitive(root.get(), key);
    if (!cJSON_IsNumber(value) || value->valuedouble != std::floor(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > 100000) return false;
    *out = value->valueint;
    return true;
}
OperationResult JsonResult(cJSON* root) {
    char* encoded = cJSON_PrintUnformatted(root);
    OperationResult result = encoded ? Ok(encoded) : Fail("Unable to encode state");
    cJSON_free(encoded); cJSON_Delete(root); return result;
}
OperationResult NetworkState() {
    const auto& state = agent_ui::SettingsView::NetworkModule().state();
    auto* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "cellular", state.cellular);
    cJSON_AddNumberToObject(root, "simSlot", state.external_slot ? 0 : 1);
    cJSON_AddStringToObject(root, "status", state.status.c_str());
    auto* saved = cJSON_AddArrayToObject(root, "saved");
    for (size_t i = 0; i < state.saved_networks.size() && i < 20; ++i) {
        auto* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "ssid", state.saved_networks[i].ssid.c_str());
        cJSON_AddBoolToObject(item, "default", state.saved_networks[i].is_default);
        cJSON_AddItemToArray(saved, item);
    }
    auto* nearby = cJSON_AddArrayToObject(root, "nearby");
    for (size_t i = 0; i < state.nearby_networks.size() && i < 20; ++i) {
        auto* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "ssid", state.nearby_networks[i].ssid.c_str());
        cJSON_AddItemToArray(nearby, item);
    }
    return JsonResult(root);
}
OperationResult BluetoothState() {
    const auto& state = agent_ui::SettingsView::BluetoothModule().state();
    auto* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", state.enabled);
    cJSON_AddBoolToObject(root, "connected", state.connection == agent_ui::bluetooth::ConnectionState::Connected);
    cJSON_AddStringToObject(root, "address", state.has_current_device ? state.current_device.address.c_str() : "");
    cJSON_AddStringToObject(root, "profile", state.audio_profile == agent_ui::bluetooth::AudioProfile::Music ? "music" :
        state.audio_profile == agent_ui::bluetooth::AudioProfile::Call ? "call" : "none");
    auto* nearby = cJSON_AddArrayToObject(root, "nearby");
    for (size_t i = 0; i < state.nearby_devices.size() && i < 20; ++i) {
        auto* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "address", state.nearby_devices[i].address.c_str());
        cJSON_AddStringToObject(item, "name", state.nearby_devices[i].name.c_str());
        cJSON_AddItemToArray(nearby, item);
    }
    return JsonResult(root);
}
bool CanDispatch(const InvokeRequest& request) {
    const auto state = Availability::Get().GetSnapshot();
    return state.available && state.generation == request.generation;
}

CapabilityState State(const char* json) { CapabilityState state; state.state_json = json; return state; }

OperationResult NetworkInvoke(const InvokeRequest& request) {
    std::string action;
    if (!StringArg(request.arguments_json, "action", &action)) return Fail("network action is required");
    namespace net = agent_ui::network;
    return UiOperations::Submit(request, [action, request, phase = 0, session = uint64_t{},
            scan_revision = uint64_t{}, failure_revision = uint64_t{}, success_revision = uint64_t{}]() mutable {
        auto& module = agent_ui::SettingsView::NetworkModule();
        if (phase == 0) {
            if (!agent_ui::SettingsView::OpenPanel(agent_ui::SettingsPanel::Network)) return Fail("settings screen is unavailable");
            session = module.session(); phase = 1; return Pending();
        }
        const auto& state = module.state();
        if (!state.mounted || module.session() != session) return Fail("network panel was closed or replaced");
        if (phase == 1) {
            if (!CanDispatch(request)) return Fail("AI availability changed before network dispatch");
            if (action == "state" || action == "saved") return NetworkState();
            if (state.scanning || state.connecting || state.network_switch_pending || state.sim_switch_pending)
                return Fail("network is busy");
            scan_revision = module.EventRevision(net::EventType::ScanFinished);
            failure_revision = module.EventRevision(net::EventType::ConnectFailed);
            success_revision = module.EventRevision(net::EventType::SimSwitchSucceeded);
            bool accepted = false;
            if (action == "scan") accepted = module.SubmitIntent(net::Intent::Scan());
            else if (action == "connect_saved") {
                int index = -1; std::string ssid;
                if (StringArg(request.arguments_json, "ssid", &ssid)) {
                    for (size_t i = 0; i < state.saved_networks.size(); ++i)
                        if (state.saved_networks[i].ssid == ssid) { index = static_cast<int>(i); break; }
                } else IntArg(request.arguments_json, "index", &index);
                if (index >= 0 && static_cast<size_t>(index) < state.saved_networks.size())
                    accepted = module.SubmitIntent(net::Intent::ConnectSaved(index));
            } else if (action == "connect") {
                std::string ssid, password;
                accepted = StringArg(request.arguments_json, "ssid", &ssid) && !ssid.empty() && ssid.size() <= 32 &&
                    StringArg(request.arguments_json, "password", &password) && password.size() <= 64 &&
                    module.SubmitIntent(net::Intent::SubmitPassword(ssid, password));
            } else if (action == "mode") {
                int mode;
                if (IntArg(request.arguments_json, "mode", &mode) && mode <= 2) {
                    const int current = state.cellular ? (state.external_slot ? 2 : 1) : 0;
                    if (current == mode) return NetworkState();
                    accepted = module.SetMode(mode);
                }
            } else if (action == "sim") {
                int slot;
                if (IntArg(request.arguments_json, "slot", &slot) && slot <= 1) {
                    if (state.cellular && (state.external_slot ? 0 : 1) == slot) return NetworkState();
                    accepted = module.SubmitIntent(net::Intent::SelectSimSlot(slot));
                }
            } else return Fail("unsupported network action");
            if (!accepted) return Fail("invalid network arguments or unavailable module");
            phase = 2; return Pending();
        }
        if (module.EventRevision(net::EventType::ConnectFailed) != failure_revision)
            return Fail(state.dialog_title.c_str());
        if (action == "scan" && module.EventRevision(net::EventType::ScanFinished) != scan_revision)
            return module.scan_succeeded() ? NetworkState() : Fail(state.status.c_str());
        if ((action == "connect" || action == "connect_saved") && state.dialog == net::Dialog::Restart)
            return Ok("{\"connected\":true,\"restartRequired\":true}");
        // The adapter schedules a reboot. Report acceptance explicitly; the new
        // connection can only be verified after the device reconnects.
        if ((action == "mode" || action == "sim") && state.network_switch_pending)
            return Ok("{\"switchScheduled\":true,\"verifiedAfterRestart\":false}");
        if ((action == "mode" || action == "sim") &&
            module.EventRevision(net::EventType::SimSwitchSucceeded) != success_revision)
            return Ok("{\"simApplied\":true,\"restartRequired\":true}");
        return Pending();
    });
}

OperationResult BluetoothInvoke(const InvokeRequest& request) {
    std::string action;
    if (!StringArg(request.arguments_json, "action", &action)) return Fail("bluetooth action is required");
    namespace bt = agent_ui::bluetooth;
    return UiOperations::Submit(request, [action, request, phase = 0, session = uint64_t{},
            busy_seen = false, expected = -1, target = std::string{}]() mutable {
        auto& module = agent_ui::SettingsView::BluetoothModule();
        if (phase == 0) {
            if (!agent_ui::SettingsView::OpenPanel(agent_ui::SettingsPanel::Bluetooth)) return Fail("settings screen is unavailable");
            session = module.session(); phase = 1; return Pending();
        }
        const auto& state = module.state();
        if (!state.mounted || module.session() != session) return Fail("Bluetooth panel was closed or replaced");
        if (phase == 1) {
            if (!CanDispatch(request)) return Fail("AI availability changed before Bluetooth dispatch");
            if (action == "state") return BluetoothState();
            if (state.scanning || state.resetting || state.connection == bt::ConnectionState::Connecting)
                return Fail("Bluetooth is busy");
            bool accepted = false;
            if (action == "enable") {
                if (IntArg(request.arguments_json, "enabled", &expected) && expected <= 1) {
                    if (state.enabled == (expected != 0)) return BluetoothState();
                    accepted = module.SubmitIntent(bt::Intent::SetEnabled(expected != 0));
                }
            } else if (action == "scan") {
                if (!state.enabled) return Fail("Enable Bluetooth before scanning");
                accepted = module.SubmitIntent(bt::Intent::Scan());
            } else if (action == "connect") {
                int index = -1;
                if (StringArg(request.arguments_json, "address", &target)) {
                    for (size_t i = 0; i < state.nearby_devices.size(); ++i)
                        if (state.nearby_devices[i].address == target) { index = static_cast<int>(i); break; }
                } else IntArg(request.arguments_json, "index", &index);
                if (index >= 0 && static_cast<size_t>(index) < state.nearby_devices.size()) {
                    target = state.nearby_devices[index].address;
                    if (state.connection == bt::ConnectionState::Connected && state.current_device.address == target) return BluetoothState();
                    accepted = state.enabled && module.SubmitIntent(bt::Intent::Connect(index));
                }
            } else if (action == "profile") {
                std::string profile;
                if (StringArg(request.arguments_json, "profile", &profile) && (profile == "music" || profile == "call")) {
                    expected = static_cast<int>(profile == "music" ? bt::AudioProfile::Music : bt::AudioProfile::Call);
                    if (static_cast<int>(state.audio_profile) == expected) return BluetoothState();
                    accepted = module.SubmitIntent(bt::Intent::SetAudioProfile(static_cast<bt::AudioProfile>(expected)));
                }
            } else if (action == "reset") accepted = module.SubmitIntent(bt::Intent::Reset());
            else return Fail("unsupported Bluetooth action");
            if (!accepted) return Fail("invalid Bluetooth arguments or unavailable device");
            phase = 2; return Pending();
        }
        if (action == "enable" && state.enabled == (expected != 0)) return BluetoothState();
        if (action == "scan") { busy_seen = busy_seen || state.scanning; if (busy_seen && !state.scanning) return BluetoothState(); }
        if (action == "connect" && state.connection == bt::ConnectionState::Connected &&
            state.has_current_device && state.current_device.address == target) return BluetoothState();
        if (action == "profile" && static_cast<int>(state.audio_profile) == expected) return BluetoothState();
        if (action == "reset") { busy_seen = busy_seen || state.resetting; if (busy_seen && !state.resetting) return BluetoothState(); }
        return Pending();
    });
}

OperationResult PhoneInvoke(const InvokeRequest& request) {
    std::string action; if (!StringArg(request.arguments_json, "action", &action)) return Fail("phone action is required");
    return UiOperations::Submit(request, [action, args = request.arguments_json, dispatched = false, revision = uint64_t{}]() mutable {
        if (!dispatched) {
            if (action == "dial") { std::string number; if (!StringArg(args, "number", &number)) return Fail("valid phone number is required"); agent_ui::Navigation::Get().Open(agent_ui::ScreenId::Phone); if (!agent_ui::PhoneView::SubmitAiDial(number.c_str())) return Fail("dial request rejected"); }
            else if (action == "hangup") { if (!agent_ui::PhoneView::SubmitAiHangup()) return Fail("no active call to hang up"); }
            else return Fail("unsupported phone action");
            revision = agent_ui::PhoneView::AiRevision(); dispatched = true; return Pending();
        }
        const char* status = agent_ui::PhoneView::AiStatus();
        if (std::strcmp(status, "failed") == 0) return Fail("dial failed");
        if (action == "dial" && std::strcmp(status, "dial_accepted") == 0) return Ok("{\"dialAccepted\":true,\"connected\":false}");
        if (action == "hangup" && std::strcmp(status, "hung_up") == 0 && agent_ui::PhoneView::AiRevision() != revision) return Ok("{\"hungUp\":true}");
        if (action == "dial" && !agent_ui::PhoneView::IsAiCallActive()) return Fail("call was cancelled");
        return Pending();
    });
}

void Add(const char* id, const char* title, const char* description, const char* schema, std::function<OperationResult(const InvokeRequest&)> invoke) {
    std::string ignored; CapabilityRegistry::Get().Register({{id,title,description,schema}, [] { return State("{\"available\":true}"); }, std::move(invoke), UiOperations::GetResult, UiOperations::Cancel}, &ignored);
}
}  // namespace

void RegisterProviders() {
    Add("system.network", "Network", "Scan Wi-Fi, connect saved networks, switch transport, and select SIM.", "{\"type\":\"object\",\"properties\":{\"action\":{\"enum\":[\"state\",\"saved\",\"scan\",\"connect_saved\",\"connect\",\"mode\",\"sim\"]},\"index\":{\"type\":\"integer\"},\"ssid\":{\"type\":\"string\"},\"password\":{\"type\":\"string\"},\"mode\":{\"enum\":[0,1,2]},\"slot\":{\"enum\":[0,1]}},\"required\":[\"action\"]}", NetworkInvoke);
    Add("system.bluetooth", "Bluetooth", "Enable, scan, connect, reset Bluetooth and choose an audio profile.", "{\"type\":\"object\",\"properties\":{\"action\":{\"enum\":[\"state\",\"enable\",\"scan\",\"connect\",\"profile\",\"reset\"]},\"enabled\":{\"enum\":[0,1]},\"index\":{\"type\":\"integer\"},\"address\":{\"type\":\"string\"},\"profile\":{\"enum\":[\"music\",\"call\"]}},\"required\":[\"action\"]}", BluetoothInvoke);
    Add("system.phone", "Phone", "Dial or hang up a cellular call. Dial acceptance is not remote connection.", "{\"type\":\"object\",\"properties\":{\"action\":{\"enum\":[\"dial\",\"hangup\"]},\"number\":{\"type\":\"string\",\"maxLength\":24}},\"required\":[\"action\"]}", PhoneInvoke);
}
void UnregisterProviders() { std::string ignored; for (const char* id : {"system.network", "system.bluetooth", "system.phone"}) CapabilityRegistry::Get().Unregister(id, &ignored); }
}  // namespace ai::system_connectivity
