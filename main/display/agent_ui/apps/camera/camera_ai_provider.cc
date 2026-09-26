#include "camera_ai_provider.h"

#include <string>
#include <utility>

#include <cJSON.h>

#include "ai/ai_capabilities.h"
#include "ai/ai_availability.h"
#include "ai/ai_ui_operation.h"
#include "core/navigation.h"
#include "camera_module.h"

namespace agent_ui::camera {
namespace {

ai::OperationResult Failure(const char* message) {
    ai::OperationResult result;
    result.status = ai::OperationStatus::Failed;
    result.error = message;
    return result;
}

ai::OperationResult Pending() {
    ai::OperationResult result;
    result.status = ai::OperationStatus::Pending;
    return result;
}

ai::OperationResult Success(const char* json = "{}") {
    ai::OperationResult result;
    result.status = ai::OperationStatus::Succeeded;
    result.result_json = json;
    return result;
}

const char* ReadString(const cJSON* object, const char* name) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : "";
}

int ReadInteger(const cJSON* object, const char* name, int fallback = -1) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}

bool ParseEffect(const char* name, EffectStyle* effect) {
    if (std::string(name) == "original") *effect = EffectStyle::Original;
    else if (std::string(name) == "mosaic") *effect = EffectStyle::Mosaic;
    else if (std::string(name) == "print") *effect = EffectStyle::PrintComic;
    else if (std::string(name) == "ascii") *effect = EffectStyle::Ascii;
    else if (std::string(name) == "black_white") *effect = EffectStyle::BlackWhite;
    else return false;
    return true;
}

ai::CapabilityState CameraState() {
    ai::CapabilityState result;
    result.available = true;
    result.enabled = true;
    result.state_json = "{\"uiRequired\":true}";
    return result;
}

ai::OperationResult Invoke(const ai::InvokeRequest& request) {
    cJSON* args = cJSON_Parse(request.arguments_json.c_str());
    if (args == nullptr || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        return Failure("camera arguments must be an object");
    }
    const std::string action = ReadString(args, "action");
    const std::string effect_name = ReadString(args, "effect");
    const int index = ReadInteger(args, "index");
    const bool dark = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "dark"));
    cJSON_Delete(args);

    EffectStyle effect = EffectStyle::Original;
    if (action == "effect" && !ParseEffect(effect_name.c_str(), &effect)) {
        return Failure("effect must be original, mosaic, print, ascii, or black_white");
    }
    if (action != "capture" && action != "save" && action != "discard" &&
        action != "effect" && action != "gallery" && action != "view" &&
        action != "delete") {
        return Failure("unsupported camera action");
    }

    bool dispatched = false;
    return ai::UiOperations::Submit(request, [request, action, effect, dark, index, dispatched,
                                             opened = false, generation = uint32_t{}]() mutable {
        if (!opened) {
            if (Navigation::Get().current() != ScreenId::Camera) Navigation::Get().Open(ScreenId::Camera);
            opened = true;
            return Pending();
        }
        if (Navigation::Get().current() != ScreenId::Camera || !Module::IsActive())
            return Failure("camera closed before operation completed");
        const ViewState* state = Module::ActiveState();
        if (state == nullptr || !state->mounted) return Pending();

        if (!dispatched) {
            const auto policy = ai::Availability::Get().GetSnapshot();
            if (!policy.available || policy.generation != request.generation)
                return Failure("AI availability changed before camera dispatch");
            if (state->saving || state->viewer_loading || state->gallery_loading)
                return Failure("camera is busy");
            if (action == "capture") {
                if (state->mode != ViewMode::Camera) return Failure("return to camera before capture");
                if (!state->preview_running || state->preview_frame == nullptr) return Pending();
                if (!Module::SubmitIntent(Intent::Capture())) return Failure("camera capture cannot start");
            } else if (action == "save") {
                if (state->mode != ViewMode::Review || !state->review_ready) return Failure("no photo is ready to save");
                if (!Module::SubmitIntent(Intent::SaveReview())) return Failure("camera save cannot start");
            } else if (action == "discard") {
                if (state->mode != ViewMode::Review) return Failure("no review photo to discard");
                if (!Module::SubmitIntent(Intent::DeleteReview())) return Failure("camera discard cannot start");
            } else if (action == "effect") {
                if (!Module::SubmitIntent(Intent::SetEffect(effect, dark))) return Failure("camera effect cannot start");
            } else if (action == "gallery") {
                if (!Module::SubmitIntent(Intent::OpenGallery())) return Failure("camera gallery cannot start");
            } else if (action == "view") {
                if (index < 0 || static_cast<size_t>(index) >= state->gallery.size()) return Failure("gallery index is out of range");
                if (!Module::SubmitIntent(Intent::OpenViewer(static_cast<size_t>(index)))) return Failure("camera viewer cannot start");
            } else if (action == "delete") {
                if (state->mode != ViewMode::Viewer) return Failure("no gallery photo is open");
                if (!Module::SubmitIntent(Intent::DeleteViewer())) return Failure("camera delete cannot start");
            }
            dispatched = true;
            generation = Module::ActiveState()->generation;
            return Pending();
        }

        state = Module::ActiveState();
        if (state == nullptr || !state->mounted) return Failure("camera UI closed before operation completed");
        if (state->generation != generation) return Failure("camera operation was superseded");
        if (state->status_code == StatusCode::CameraStartupFailed ||
            state->status_code == StatusCode::SaveFailed ||
            state->status_code == StatusCode::DeleteFailed ||
            (state->status_code == StatusCode::BackendMessage &&
             !state->status.empty())) return Failure(state->status.c_str());
        if (action == "capture" && state->mode == ViewMode::Review && state->review_ready) return Success("{\"reviewReady\":true}");
        if (action == "save" && !state->saving && state->mode == ViewMode::Camera) return Success("{\"saved\":true}");
        if (action == "discard" && state->mode == ViewMode::Camera && !state->review_ready) return Success("{\"discarded\":true}");
        if (action == "effect" && state->effect_style == effect && state->effect_dark_mode == dark) return Success("{\"effectApplied\":true}");
        if (action == "gallery" && state->mode == ViewMode::Gallery && !state->gallery_loading) {
            cJSON* root = cJSON_CreateObject();
            auto* photos = cJSON_AddArrayToObject(root, "photos");
            cJSON_AddNumberToObject(root, "total", state->gallery.size());
            for (size_t i = 0; i < state->gallery.size() && i < 24; ++i) {
                auto* item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "index", i);
                cJSON_AddStringToObject(item, "name", state->gallery[i].name.c_str());
                cJSON_AddStringToObject(item, "path", state->gallery[i].path.c_str());
                cJSON_AddItemToArray(photos, item);
            }
            char* encoded = cJSON_PrintUnformatted(root);
            auto result = Success(encoded ? encoded : "{}");
            cJSON_free(encoded); cJSON_Delete(root); return result;
        }
        if (action == "view" && state->mode == ViewMode::Viewer && !state->viewer_loading) return Success("{\"viewerReady\":true}");
        if (action == "delete" && state->mode == ViewMode::Gallery && !state->gallery_loading) return Success("{\"deleted\":true}");
        return Pending();
    });
}

}  // namespace

void RegisterAiProvider() {
    ai::CapabilityProvider provider;
    provider.descriptor = {
        "camera.control", "Camera", "Capture, save, discard, edit, browse, view, and delete camera photos.",
        "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"capture\",\"save\",\"discard\",\"effect\",\"gallery\",\"view\",\"delete\"]},\"effect\":{\"type\":\"string\",\"enum\":[\"original\",\"mosaic\",\"print\",\"ascii\",\"black_white\"]},\"dark\":{\"type\":\"boolean\"},\"index\":{\"type\":\"integer\",\"minimum\":0}},\"required\":[\"action\"]}"};
    provider.get_state = CameraState;
    provider.invoke = Invoke;
    provider.get_result = ai::UiOperations::GetResult;
    provider.cancel = ai::UiOperations::Cancel;
    std::string ignored;
    ai::CapabilityRegistry::Get().Register(std::move(provider), &ignored);
}

void UnregisterAiProvider() {
    std::string ignored;
    ai::CapabilityRegistry::Get().Unregister("camera.control", &ignored);
}

}  // namespace agent_ui::camera
