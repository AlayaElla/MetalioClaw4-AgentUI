#include "external_app_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <vector>

#include <esp_elf.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "agent_ui/core/fonts.h"
#include "agent_ui/core/theme.h"
#include "agent_ui/components/pet_renderer.h"
#include "agent_ui/components/ui_components.h"
#include "font_awesome.h"
#include "metalio_app_api.h"
#include "src/draw/lv_image_decoder_private.h"

namespace agent_ui::external_apps {
namespace {

constexpr char kTag[] = "ExternalAppRuntime";
constexpr size_t kMaxElfBytes = 4 * 1024 * 1024;
constexpr size_t kMaxLabelBytes = 512;
constexpr size_t kMaxAssetBytes = 8 * 1024 * 1024;
constexpr uint16_t kElfMachineRiscV = 243;
constexpr uint32_t kRelocationStackBytes = 6 * 1024;
constexpr uint32_t kMinimumIntervalMs = 16;
constexpr uint32_t kMaximumIntervalMs = 60 * 1000;
constexpr uint32_t kMaximumPetKeyframes = 64;

bool IsSafeRelativePath(const char* raw_path) {
    if (raw_path == nullptr) return false;
    const size_t length = strnlen(raw_path, 256);
    if (length == 0 || length == 256 || raw_path[0] == '/' || raw_path[0] == '\\') {
        return false;
    }
    std::string path(raw_path, length);
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
        return false;
    }
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string component = path.substr(start, end - start);
        if (component.empty() || component == "." || component == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool IsImageFile(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return std::tolower(value); });
    return extension == ".png" || extension == ".jpg" ||
           extension == ".jpeg" || extension == ".sjpg";
}

bool IsValidRect(int16_t x, int16_t y, int16_t width, int16_t height) {
    if (x < 0 || y < 0 || width <= 0 || height <= 0) return false;
    return static_cast<int32_t>(x) + width <= metrics::kDisplaySize &&
           static_cast<int32_t>(y) + height <= metrics::kBottomActionContentHeight;
}

const lv_font_t* ResolveFont(metalio_app_font_t font) {
    switch (font) {
        case METALIO_APP_FONT_SMALL:
            return fonts::Small();
        case METALIO_APP_FONT_MEDIUM_BOLD:
            return fonts::MediumBold();
        case METALIO_APP_FONT_MEDIUM:
        default:
            return fonts::Medium();
    }
}

void OnImageDeleted(lv_event_t* event) {
    delete static_cast<std::string*>(lv_event_get_user_data(event));
}

}  // namespace

struct ExternalLabel {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
};

struct ExternalInterval {
    lv_timer_t* timer = nullptr;
    metalio_app_callback_t callback = nullptr;
    void* app_context = nullptr;
};

struct ExternalAction {
    lv_obj_t* button = nullptr;
    metalio_app_callback_t callback = nullptr;
    void* app_context = nullptr;
};

struct ExternalPet {
    metalio_app_widget_t id = 0;
    std::string texture_path;
    lv_image_decoder_dsc_t decoder{};
    bool decoder_open = false;
    lv_image_dsc_t texture{};
    std::unique_ptr<pet::Renderer> renderer;
    std::string clip_id;
    std::vector<pet::Keyframe> keyframes;
    pet::Clip clip{};

    ~ExternalPet() {
        renderer.reset();
        if (decoder_open) {
            lv_image_decoder_close(&decoder);
            decoder_open = false;
        }
    }
};

struct Runtime::State {
    AppInfo app;
    lv_obj_t* content = nullptr;
    lv_obj_t* actions = nullptr;
    uint8_t* elf_bytes = nullptr;
    size_t elf_size = 0;
    esp_elf_t elf{};
    bool elf_initialized = false;
    bool paused = false;
    metalio_app_widget_t next_widget_id = 1;
    std::vector<ExternalLabel> labels;
    std::vector<std::unique_ptr<ExternalInterval>> intervals;
    std::vector<std::unique_ptr<ExternalAction>> action_callbacks;
    std::vector<std::unique_ptr<ExternalPet>> pets;
    metalio_app_host_api_t api{};
    metalio_app_launch_context_t launch_context{};
};

namespace {

struct RelocationRequest {
    Runtime::State* state = nullptr;
    SemaphoreHandle_t completion = nullptr;
    int init_result = -1;
    int relocate_result = -1;
};

void RelocateOnInternalStack(void* argument) {
    auto* request = static_cast<RelocationRequest*>(argument);
    request->init_result = esp_elf_init(&request->state->elf);
    if (request->init_result == 0) {
        request->state->elf_initialized = true;
        request->relocate_result = esp_elf_relocate(
            &request->state->elf, request->state->elf_bytes);
    }
    xSemaphoreGive(request->completion);
    // The caller owns the WithCaps task and deletes it after this task is
    // suspended. This avoids the self-delete cleanup task described by IDF.
    vTaskSuspend(nullptr);
}

bool RelocateWithInternalStack(Runtime::State* state, std::string* error) {
    StaticSemaphore_t completion_storage{};
    SemaphoreHandle_t completion = xSemaphoreCreateBinaryStatic(&completion_storage);
    if (completion == nullptr) {
        if (error != nullptr) *error = "无法创建 ELF 重定位同步对象";
        return false;
    }
    RelocationRequest request{
        .state = state,
        .completion = completion,
    };
    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        RelocateOnInternalStack, "elf_relocate", kRelocationStackBytes,
        &request, tskIDLE_PRIORITY + 5, &task, xPortGetCoreID(),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        if (error != nullptr) *error = "内部 RAM 不足，无法创建 ELF 重定位任务";
        return false;
    }

    xSemaphoreTake(completion, portMAX_DELAY);
    vTaskDeleteWithCaps(task);
    if (request.init_result != 0) {
        if (error != nullptr) *error = "ELF 加载器初始化失败";
        return false;
    }
    if (request.relocate_result != 0) {
        if (error != nullptr) *error = "ELF 重定位失败；请检查 ABI 与目标芯片";
        return false;
    }
    return true;
}

Runtime::State* CheckedState(void* host_context) {
    auto* state = static_cast<Runtime::State*>(host_context);
    return state != nullptr && state->content != nullptr ? state : nullptr;
}

int SetBackground(void* host_context, uint32_t rgb888) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    lv_obj_set_style_bg_color(state->content, lv_color_hex(rgb888 & 0xFFFFFFU),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->content, LV_OPA_COVER, LV_PART_MAIN);
    return 0;
}

lv_obj_t* CreateLabel(Runtime::State* state, const char* text, int16_t x,
                      int16_t y, int16_t width, int16_t height,
                      uint32_t rgb888, metalio_app_font_t font) {
    if (state == nullptr || text == nullptr || !IsValidRect(x, y, width, height) ||
        strnlen(text, kMaxLabelBytes + 1) > kMaxLabelBytes) {
        return nullptr;
    }
    lv_obj_t* label = lv_label_create(state->content);
    if (label == nullptr) return nullptr;
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_color(label, lv_color_hex(rgb888 & 0xFFFFFFU),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(label, ResolveFont(font), LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

int AddLabel(void* host_context, const char* text, int16_t x, int16_t y,
             int16_t width, int16_t height, uint32_t rgb888,
             metalio_app_font_t font) {
    Runtime::State* state = CheckedState(host_context);
    return CreateLabel(state, text, x, y, width, height, rgb888, font) != nullptr
               ? 0
               : -1;
}

int AddImage(void* host_context, const char* asset_relative_path, int16_t x,
             int16_t y, int16_t width, int16_t height) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || !IsValidRect(x, y, width, height) ||
        !IsSafeRelativePath(asset_relative_path)) {
        return -1;
    }
    const std::string posix_path = state->app.root_path + "/" + asset_relative_path;
    struct stat info {};
    if (!IsImageFile(posix_path) || stat(posix_path.c_str(), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<size_t>(info.st_size) > kMaxAssetBytes) {
        return -1;
    }

    auto* lv_path = new (std::nothrow) std::string("S:" + posix_path);
    if (lv_path == nullptr) return -1;
    lv_obj_t* image = lv_image_create(state->content);
    if (image == nullptr) {
        delete lv_path;
        return -1;
    }
    lv_obj_set_pos(image, x, y);
    lv_obj_set_size(image, width, height);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(image, lv_path->c_str());
    lv_obj_add_event_cb(image, OnImageDeleted, LV_EVENT_DELETE, lv_path);
    return 0;
}

int AddLabelEx(void* host_context, const char* text, int16_t x, int16_t y,
               int16_t width, int16_t height, uint32_t rgb888,
               metalio_app_font_t font, metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || widget == nullptr) return -1;
    lv_obj_t* label = CreateLabel(state, text, x, y, width, height, rgb888, font);
    if (label == nullptr) return -1;
    const metalio_app_widget_t id = state->next_widget_id++;
    state->labels.push_back({id, label});
    *widget = id;
    return 0;
}

int SetLabelText(void* host_context, metalio_app_widget_t widget,
                 const char* text) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || text == nullptr ||
        strnlen(text, kMaxLabelBytes + 1) > kMaxLabelBytes) {
        return -1;
    }
    const auto found = std::find_if(
        state->labels.begin(), state->labels.end(),
        [widget](const ExternalLabel& label) { return label.id == widget; });
    if (found == state->labels.end() || found->object == nullptr ||
        !lv_obj_is_valid(found->object)) {
        return -1;
    }
    lv_label_set_text(found->object, text);
    return 0;
}

void OnInterval(lv_timer_t* timer) {
    auto* interval = static_cast<ExternalInterval*>(lv_timer_get_user_data(timer));
    if (interval != nullptr && interval->callback != nullptr) {
        interval->callback(interval->app_context);
    }
}

int SetInterval(void* host_context, uint32_t period_ms,
                metalio_app_callback_t callback, void* app_context) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || callback == nullptr ||
        period_ms < kMinimumIntervalMs || period_ms > kMaximumIntervalMs) {
        return -1;
    }
    auto interval = std::make_unique<ExternalInterval>();
    interval->callback = callback;
    interval->app_context = app_context;
    interval->timer = lv_timer_create(OnInterval, period_ms, interval.get());
    if (interval->timer == nullptr) return -1;
    if (state->paused) lv_timer_pause(interval->timer);
    state->intervals.push_back(std::move(interval));
    return 0;
}

void OnActionClicked(lv_event_t* event) {
    auto* action = static_cast<ExternalAction*>(lv_event_get_user_data(event));
    if (action != nullptr && action->callback != nullptr) {
        action->callback(action->app_context);
    }
}

const char* ResolveActionIcon(metalio_app_action_icon_t icon) {
    switch (icon) {
        case METALIO_APP_ACTION_WIND:
            return FONT_AWESOME_WIND;
        case METALIO_APP_ACTION_HEART:
            return FONT_AWESOME_HEART;
        case METALIO_APP_ACTION_REFRESH:
            return FONT_AWESOME_ARROWS_ROTATE;
        case METALIO_APP_ACTION_PLAY:
            return FONT_AWESOME_PLAY;
        case METALIO_APP_ACTION_INFO:
        default:
            return FONT_AWESOME_CIRCLE_INFO;
    }
}

int AddAction(void* host_context, metalio_app_action_icon_t icon,
              const char* label, metalio_app_callback_t callback,
              void* app_context) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || state->actions == nullptr || label == nullptr ||
        callback == nullptr || strnlen(label, 33) > 32) {
        return -1;
    }
    auto action = std::make_unique<ExternalAction>();
    action->callback = callback;
    action->app_context = app_context;
    const auto parts = ui_components::AddBottomActionButton(
        state->actions, ResolveActionIcon(icon), label, OnActionClicked,
        action.get());
    if (parts.root == nullptr) return -1;
    action->button = parts.root;
    state->action_callbacks.push_back(std::move(action));
    return 0;
}

pet::Vec2 ConvertVec(metalio_pet_vec2_t value) {
    return {value.x, value.y};
}

pet::Pose ConvertPose(const metalio_pet_pose_t& source) {
    pet::Pose destination;
    destination.root_translation = ConvertVec(source.root_translation);
    destination.root_rotation_degrees = source.root_rotation_degrees;
    destination.root_scale = ConvertVec(source.root_scale);
    for (size_t index = 0; index < pet::kMaxBodyVertices; ++index) {
        destination.body_vertex_offsets[index] =
            ConvertVec(source.body_vertex_offsets[index]);
    }
    for (size_t index = 0; index < pet::kMaxLimbs; ++index) {
        destination.limbs[index].control1_offset =
            ConvertVec(source.limbs[index].control1_offset);
        destination.limbs[index].control2_offset =
            ConvertVec(source.limbs[index].control2_offset);
        destination.limbs[index].end_offset =
            ConvertVec(source.limbs[index].end_offset);
        destination.limbs[index].width_scale = source.limbs[index].width_scale;
        destination.limbs[index].opacity = source.limbs[index].opacity;
    }
    return destination;
}

pet::Rig ConvertRig(const metalio_pet_rig_t& source) {
    pet::Rig destination;
    destination.body.columns = source.columns;
    destination.body.rows = source.rows;
    destination.body.destination = {
        source.destination_x, source.destination_y, source.destination_width,
        source.destination_height};
    destination.body.uv = {
        source.uv_x, source.uv_y, source.uv_width, source.uv_height};
    destination.root_pivot = ConvertVec(source.root_pivot);
    destination.limb_count = source.limb_count;
    for (size_t index = 0; index < pet::kMaxLimbs; ++index) {
        const metalio_pet_limb_rig_t& input = source.limbs[index];
        pet::LimbRig& output = destination.limbs[index];
        output.anchor_vertex = input.anchor_vertex;
        output.control1 = ConvertVec(input.control1);
        output.control2 = ConvertVec(input.control2);
        output.end = ConvertVec(input.end);
        output.width = input.width;
        output.color = {
            static_cast<uint8_t>((input.rgba8888 >> 24U) & 0xFFU),
            static_cast<uint8_t>((input.rgba8888 >> 16U) & 0xFFU),
            static_cast<uint8_t>((input.rgba8888 >> 8U) & 0xFFU),
            static_cast<uint8_t>(input.rgba8888 & 0xFFU),
        };
        output.segments = input.segments;
        output.draw_behind_body = input.draw_behind_body != 0;
    }
    return destination;
}

ExternalPet* FindPet(Runtime::State* state, metalio_app_widget_t widget) {
    if (state == nullptr) return nullptr;
    const auto found = std::find_if(
        state->pets.begin(), state->pets.end(),
        [widget](const std::unique_ptr<ExternalPet>& pet_widget) {
            return pet_widget != nullptr && pet_widget->id == widget;
        });
    return found == state->pets.end() ? nullptr : found->get();
}

int AddPet(void* host_context, const char* texture_asset_relative_path,
           int16_t x, int16_t y, int16_t width, int16_t height,
           const metalio_pet_rig_t* rig, metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || rig == nullptr || widget == nullptr ||
        !IsValidRect(x, y, width, height) ||
        !IsSafeRelativePath(texture_asset_relative_path)) {
        return -1;
    }
    const std::string asset_path =
        state->app.root_path + "/" + texture_asset_relative_path;
    struct stat info {};
    if (!IsImageFile(asset_path) || stat(asset_path.c_str(), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<size_t>(info.st_size) > kMaxAssetBytes) {
        return -1;
    }

    auto pet_widget = std::make_unique<ExternalPet>();
    pet_widget->texture_path = "S:" + asset_path;
    lv_image_decoder_args_t decoder_args{};
    decoder_args.no_cache = true;
    decoder_args.premultiply = false;
    if (lv_image_decoder_open(&pet_widget->decoder,
                              pet_widget->texture_path.c_str(),
                              &decoder_args) != LV_RESULT_OK ||
        pet_widget->decoder.decoded == nullptr) {
        return -1;
    }
    pet_widget->decoder_open = true;
    lv_draw_buf_to_image(pet_widget->decoder.decoded, &pet_widget->texture);

    pet_widget->renderer =
        std::make_unique<pet::Renderer>(state->content, width, height);
    if (pet_widget->renderer == nullptr ||
        !pet_widget->renderer->SetTexture(&pet_widget->texture) ||
        !pet_widget->renderer->SetRig(ConvertRig(*rig))) {
        return -1;
    }
    lv_obj_set_pos(pet_widget->renderer->Object(), x, y);
    pet_widget->id = state->next_widget_id++;
    *widget = pet_widget->id;
    state->pets.push_back(std::move(pet_widget));
    return 0;
}

int PetPlay(void* host_context, metalio_app_widget_t widget,
            const metalio_pet_clip_t* clip, uint32_t frame_period_ms) {
    Runtime::State* state = CheckedState(host_context);
    ExternalPet* pet_widget = FindPet(state, widget);
    if (pet_widget == nullptr || pet_widget->renderer == nullptr || clip == nullptr ||
        clip->id == nullptr || clip->keyframes == nullptr ||
        clip->keyframe_count == 0 || clip->keyframe_count > kMaximumPetKeyframes ||
        strnlen(clip->id, 65) > 64) {
        return -1;
    }
    pet_widget->renderer->StopPlayback();
    pet_widget->clip_id = clip->id;
    pet_widget->keyframes.clear();
    pet_widget->keyframes.reserve(clip->keyframe_count);
    for (uint32_t index = 0; index < clip->keyframe_count; ++index) {
        const metalio_pet_keyframe_t& input = clip->keyframes[index];
        pet_widget->keyframes.push_back({
            .time_ms = input.time_ms,
            .pose = ConvertPose(input.pose),
            .curve_to_next = {input.curve_y1, input.curve_y2},
        });
    }
    pet_widget->clip = {
        .id = pet_widget->clip_id.c_str(),
        .keyframes = pet_widget->keyframes.data(),
        .keyframe_count = pet_widget->keyframes.size(),
        .duration_ms = clip->duration_ms,
        .loop = clip->loop != 0,
    };
    pet_widget->renderer->SetFramePeriodMs(frame_period_ms);
    pet_widget->renderer->SetRenderingPaused(state->paused);
    pet_widget->renderer->ResetStats();
    return pet_widget->renderer->Play(&pet_widget->clip) ? 0 : -1;
}

int PetSetDebug(void* host_context, metalio_app_widget_t widget,
                uint8_t enabled) {
    ExternalPet* pet_widget = FindPet(CheckedState(host_context), widget);
    if (pet_widget == nullptr || pet_widget->renderer == nullptr) return -1;
    pet_widget->renderer->SetDebugOverlayEnabled(enabled != 0);
    return 0;
}

int PetGetStats(void* host_context, metalio_app_widget_t widget,
                metalio_pet_render_stats_t* stats) {
    ExternalPet* pet_widget = FindPet(CheckedState(host_context), widget);
    if (pet_widget == nullptr || pet_widget->renderer == nullptr || stats == nullptr) {
        return -1;
    }
    const pet::RenderStats& source = pet_widget->renderer->Stats();
    *stats = {
        .frame_count = source.frame_count,
        .last_render_us = source.last_render_us,
        .last_raster_us = source.last_raster_us,
        .average_render_us = source.AverageRenderUs(),
        .max_render_us = source.max_render_us,
        .budget_overrun_count = source.budget_overrun_count,
        .missed_frame_count = source.missed_frame_count,
    };
    return 0;
}

bool HasValidElfHeader(const uint8_t* bytes, size_t size) {
    if (bytes == nullptr || size < 20) return false;
    return bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' &&
           bytes[3] == 'F' && bytes[4] == 1 && bytes[5] == 1 &&
           static_cast<uint16_t>(bytes[18] | (bytes[19] << 8U)) ==
               kElfMachineRiscV;
}

}  // namespace

Runtime& Runtime::Get() {
    static Runtime instance;
    return instance;
}

bool Runtime::Launch(const AppInfo& app, lv_obj_t* content, lv_obj_t* actions,
                     std::string* error) {
    Unload();
    if (content == nullptr || actions == nullptr) {
        if (error != nullptr) *error = "外部 App 容器无效";
        return false;
    }

    struct stat info {};
    if (stat(app.entry_path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 20 || static_cast<size_t>(info.st_size) > kMaxElfBytes) {
        if (error != nullptr) *error = "ELF 入口不存在或大小超限";
        return false;
    }

    auto* state = new (std::nothrow) State();
    if (state == nullptr) {
        if (error != nullptr) *error = "无法分配 App 运行状态";
        return false;
    }
    state->app = app;
    state->content = content;
    state->actions = actions;
    state->elf_size = static_cast<size_t>(info.st_size);
    state->elf_bytes = static_cast<uint8_t*>(
        heap_caps_malloc(state->elf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (state->elf_bytes == nullptr) {
        delete state;
        if (error != nullptr) *error = "PSRAM 不足，无法载入 ELF";
        return false;
    }

    FILE* file = fopen(app.entry_path.c_str(), "rb");
    const bool read_okay = file != nullptr &&
                           fread(state->elf_bytes, 1, state->elf_size, file) ==
                               state->elf_size;
    if (file != nullptr) fclose(file);
    if (!read_okay || !HasValidElfHeader(state->elf_bytes, state->elf_size)) {
        heap_caps_free(state->elf_bytes);
        delete state;
        if (error != nullptr) *error = "ELF 文件无效或不是 ESP32-P4 RISC-V ELF";
        return false;
    }

    state_ = state;
    if (!RelocateWithInternalStack(state, error)) {
        Unload();
        return false;
    }
    const void* entry = reinterpret_cast<const void*>(state->elf.entry);
    if (!esp_ptr_internal(entry) || !esp_ptr_executable(entry)) {
        ESP_LOGE(kTag, "ELF entry %p is not in internal executable memory",
                 reinterpret_cast<void*>(state->elf.entry));
        if (error != nullptr) {
            *error = "ELF 入口未加载到内部可执行内存，已阻止启动";
        }
        Unload();
        return false;
    }

    state->api = {
        .abi_version = METALIO_APP_ABI_VERSION,
        .struct_size = sizeof(metalio_app_host_api_t),
        .set_background = SetBackground,
        .add_label = AddLabel,
        .add_image = AddImage,
        .add_label_ex = AddLabelEx,
        .set_label_text = SetLabelText,
        .set_interval = SetInterval,
        .add_action = AddAction,
        .add_pet = AddPet,
        .pet_play = PetPlay,
        .pet_set_debug = PetSetDebug,
        .pet_get_stats = PetGetStats,
    };
    state->launch_context = {
        .abi_version = METALIO_APP_ABI_VERSION,
        .struct_size = sizeof(metalio_app_launch_context_t),
        .host_context = state,
        .content_width = metrics::kDisplaySize,
        .content_height = metrics::kBottomActionContentHeight,
    };
    char* arguments[] = {
        reinterpret_cast<char*>(&state->api),
        reinterpret_cast<char*>(&state->launch_context),
    };
    if (esp_elf_request(&state->elf, 0, 2, arguments) != 0) {
        if (error != nullptr) *error = "外部 App 执行失败";
        Unload();
        return false;
    }
    ESP_LOGI(kTag, "Launched %s (%u bytes)", app.id.c_str(),
             static_cast<unsigned>(state->elf_size));
    return true;
}

void Runtime::SetPaused(bool paused) {
    if (state_ == nullptr || state_->paused == paused) return;
    state_->paused = paused;
    for (const auto& interval : state_->intervals) {
        if (interval == nullptr || interval->timer == nullptr) continue;
        if (paused) {
            lv_timer_pause(interval->timer);
        } else {
            lv_timer_resume(interval->timer);
        }
    }
    for (const auto& pet_widget : state_->pets) {
        if (pet_widget != nullptr && pet_widget->renderer != nullptr) {
            pet_widget->renderer->SetRenderingPaused(paused);
        }
    }
}

void Runtime::Unload() {
    if (state_ == nullptr) return;
    for (const auto& interval : state_->intervals) {
        if (interval != nullptr && interval->timer != nullptr) {
            lv_timer_delete(interval->timer);
            interval->timer = nullptr;
        }
    }
    state_->intervals.clear();
    state_->pets.clear();
    for (const auto& action : state_->action_callbacks) {
        if (action != nullptr && action->button != nullptr &&
            lv_obj_is_valid(action->button)) {
            lv_obj_remove_event_cb_with_user_data(
                action->button, OnActionClicked, action.get());
        }
    }
    state_->action_callbacks.clear();
    if (state_->elf_initialized) esp_elf_deinit(&state_->elf);
    if (state_->elf_bytes != nullptr) heap_caps_free(state_->elf_bytes);
    ESP_LOGI(kTag, "Unloaded %s", state_->app.id.c_str());
    delete state_;
    state_ = nullptr;
}

}  // namespace agent_ui::external_apps
