#include "external_app_runtime.h"
#include "external_http_service.h"
#include "external_media_service.h"
#include "external_recording_service.h"
#include "external_app_symbols.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <esp_elf.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "agent_ui/core/fonts.h"
#include "agent_ui/core/theme.h"
#include "agent_ui/core/ui_utils.h"
#include "agent_ui/components/pet_renderer.h"
#include "agent_ui/components/haptic_feedback.h"
#include "agent_ui/components/ui_components.h"
#include "boards/metalio-claw-4/sc7a20_motion.h"
#include "board.h"
#include "font_awesome.h"
#include "metalio_app_api.h"
#include "src/draw/lv_image_decoder_private.h"
#include "system_info.h"

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
constexpr uint8_t kMaximumGridAxis = 8;
constexpr uint32_t kExternalServiceResetTimeoutMs = 12000;
constexpr char kAppDataRoot[] = "/sdcard/metalio/app-data";
constexpr size_t kMaxAppStorageBytes = 64 * 1024;
constexpr uint32_t kMaximumControlOptions = 48;
constexpr int kActionBarTop = 14;
constexpr int kActionBarLeft = 30;
constexpr int kPickerX = 120;
constexpr int kPickerWidth = 570;
constexpr int kPickerHeight = 82;
constexpr int kPickerStep = 204;
constexpr float kPickerMaxVelocity = 3.1f;
constexpr float kPickerReleaseBoost = 1.3f;
constexpr float kPickerFriction = 0.95f;
constexpr float kPickerStopVelocity = 0.025f;
constexpr float kPickerMinReleaseVelocity = 0.06f;
constexpr float kPickerSnapDurationMs = 240.0f;
constexpr uint32_t kPickerMotionPeriodMs = 16;
constexpr int kPickerDragThreshold = 10;

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

bool EnsureDirectory(const std::string& path) {
    struct stat info {};
    if (stat(path.c_str(), &info) == 0) return S_ISDIR(info.st_mode);
    return errno == ENOENT && mkdir(path.c_str(), 0755) == 0;
}

bool EnsureAppDataDirectories(const std::string& app_root,
                              const char* relative_path) {
    if (!EnsureDirectory("/sdcard/metalio") ||
        !EnsureDirectory(kAppDataRoot) || !EnsureDirectory(app_root)) {
        return false;
    }
    std::string path = app_root;
    const char* cursor = relative_path;
    while (const char* slash = std::strchr(cursor, '/')) {
        path.append("/").append(cursor, static_cast<size_t>(slash - cursor));
        if (!EnsureDirectory(path)) return false;
        cursor = slash + 1;
    }
    return true;
}

int ReadAppFile(const std::string& path, uint8_t* buffer, uint32_t capacity,
                uint32_t* data_size) {
    if (data_size == nullptr) return METALIO_APP_STORAGE_ERROR_INVALID;
    *data_size = 0;
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        return errno == ENOENT ? METALIO_APP_STORAGE_ERROR_NOT_FOUND
                               : METALIO_APP_STORAGE_ERROR_IO;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        return METALIO_APP_STORAGE_ERROR_IO;
    }
    const size_t size = static_cast<size_t>(info.st_size);
    if (size > kMaxAppStorageBytes) {
        return METALIO_APP_STORAGE_ERROR_TOO_LARGE;
    }
    *data_size = static_cast<uint32_t>(size);
    if ((size != 0 && buffer == nullptr) || capacity < size) {
        return METALIO_APP_STORAGE_ERROR_BUFFER_TOO_SMALL;
    }
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return METALIO_APP_STORAGE_ERROR_IO;
    const bool okay = size == 0 || std::fread(buffer, 1, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    return okay && closed ? METALIO_APP_STORAGE_OK
                          : METALIO_APP_STORAGE_ERROR_IO;
}

int WriteAppFile(const std::string& app_root, const char* relative_path,
                 const uint8_t* data, uint32_t data_size) {
    if ((data_size != 0 && data == nullptr) ||
        data_size > kMaxAppStorageBytes ||
        !EnsureAppDataDirectories(app_root, relative_path)) {
        return data_size > kMaxAppStorageBytes
                   ? METALIO_APP_STORAGE_ERROR_TOO_LARGE
                   : METALIO_APP_STORAGE_ERROR_IO;
    }

    const std::string path = app_root + "/" + relative_path;
    const std::string temporary = path + ".new";
    const std::string backup = path + ".bak";
    unlink(temporary.c_str());
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) return METALIO_APP_STORAGE_ERROR_IO;
    const bool written = data_size == 0 ||
                         std::fwrite(data, 1, data_size, file) == data_size;
    const bool flushed = written && std::fflush(file) == 0;
    const bool synced = flushed && fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !flushed || !synced || !closed) {
        unlink(temporary.c_str());
        return METALIO_APP_STORAGE_ERROR_IO;
    }

    struct stat existing {};
    const bool had_existing = stat(path.c_str(), &existing) == 0;
    unlink(backup.c_str());
    if (had_existing && rename(path.c_str(), backup.c_str()) != 0) {
        unlink(temporary.c_str());
        return METALIO_APP_STORAGE_ERROR_IO;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        if (had_existing) rename(backup.c_str(), path.c_str());
        unlink(temporary.c_str());
        return METALIO_APP_STORAGE_ERROR_IO;
    }
    if (had_existing) unlink(backup.c_str());
    return METALIO_APP_STORAGE_OK;
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
        case METALIO_APP_FONT_LARGE:
            return fonts::Large();
        case METALIO_APP_FONT_LARGE_BOLD:
            return fonts::LargeBold();
        case METALIO_APP_FONT_SMALL_BOLD:
            return fonts::SmallBold();
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

void CopyText(char* destination, size_t capacity, const char* source) {
    if (destination == nullptr || capacity == 0) return;
    if (source == nullptr) source = "";
    std::snprintf(destination, capacity, "%s", source);
}

}  // namespace

struct ExternalLabel {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
};

struct ExternalImage {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
    std::string* source = nullptr;
};

struct ExternalBar {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
};

struct ExternalRect {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
};

struct ExternalButton {
    metalio_app_widget_t id = 0;
    metalio_app_widget_t list_id = 0;
    lv_obj_t* button = nullptr;
    lv_obj_t* label = nullptr;
    metalio_app_callback_t callback = nullptr;
    void* app_context = nullptr;
};

struct ExternalGrid {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
    int16_t width = 0;
    int16_t height = 0;
    uint8_t columns = 0;
    uint8_t rows = 0;
    uint8_t column_gap = 0;
    uint8_t row_gap = 0;
};

struct ExternalIcon {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
};

struct ExternalList {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
};

struct ExternalSlider {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
    metalio_app_value_callback_t callback = nullptr;
    void* app_context = nullptr;
    int32_t last_pressed_value = 0;
    bool pointer_down = false;
    bool restoring = false;
    bool updating = false;
};

struct ExternalSegment;

struct ExternalSegmentItem {
    ExternalSegment* owner = nullptr;
    uint8_t index = 0;
    lv_obj_t* button = nullptr;
    lv_obj_t* label = nullptr;
};

struct ExternalSegment {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
    uint8_t selected = 0;
    metalio_app_selection_callback_t callback = nullptr;
    void* app_context = nullptr;
    std::vector<ExternalSegmentItem> items;
};

struct ExternalPicker {
    metalio_app_widget_t id = 0;
    lv_obj_t* object = nullptr;
    std::array<lv_obj_t*, 5> items{};
    std::array<lv_obj_t*, 5> numbers{};
    std::array<lv_obj_t*, 5> names{};
    std::vector<std::string> labels;
    uint32_t selected = 0;
    uint32_t notified = 0;
    metalio_app_selection_callback_t callback = nullptr;
    void* app_context = nullptr;
    float offset = 0.0f;
    float position = 0.0f;
    float velocity = 0.0f;
    float snap_start_offset = 0.0f;
    float snap_target_position = 0.0f;
    float snap_elapsed_ms = 0.0f;
    int16_t pointer_start_x = 0;
    int16_t pointer_last_x = 0;
    int64_t pointer_last_us = 0;
    int64_t motion_last_us = 0;
    lv_timer_t* motion_timer = nullptr;
    bool pointer_active = false;
    bool pointer_moved = false;
    bool snapping = false;

    ~ExternalPicker() {
        if (motion_timer != nullptr) {
            lv_timer_delete(motion_timer);
            motion_timer = nullptr;
        }
    }
};

struct ExternalInterval {
    lv_timer_t* timer = nullptr;
    metalio_app_callback_t callback = nullptr;
    void* app_context = nullptr;
};

struct ExternalAction {
    lv_obj_t* button = nullptr;
    lv_obj_t* icon = nullptr;
    lv_obj_t* label = nullptr;
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
    std::vector<ExternalImage> images;
    std::vector<ExternalBar> bars;
    std::vector<ExternalRect> rectangles;
    std::vector<std::unique_ptr<ExternalButton>> buttons;
    std::vector<ExternalGrid> grids;
    std::vector<ExternalIcon> icons;
    std::vector<ExternalList> lists;
    std::vector<std::unique_ptr<ExternalSlider>> sliders;
    std::vector<std::unique_ptr<ExternalSegment>> segments;
    std::vector<std::unique_ptr<ExternalPicker>> pickers;
    std::vector<std::unique_ptr<ExternalInterval>> intervals;
    std::vector<std::unique_ptr<ExternalAction>> action_callbacks;
    std::vector<std::unique_ptr<ExternalPet>> pets;
    metalio_app_swipe_callback_t swipe_callback = nullptr;
    void* swipe_app_context = nullptr;
    bool swipe_event_attached = false;
    metalio_app_theme_callback_t theme_callback = nullptr;
    void* theme_app_context = nullptr;
    lv_timer_t* theme_timer = nullptr;
    uint64_t theme_signature = 0;
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

metalio_app_capabilities_t GetCapabilities(void* host_context) {
    if (CheckedState(host_context) == nullptr) return 0;
    return METALIO_APP_CAP_HAPTICS |
           METALIO_APP_CAP_MOTION_ACCELEROMETER |
           METALIO_APP_CAP_MOTION_TILT |
           METALIO_APP_CAP_MEDIA_HLS |
           METALIO_APP_CAP_MEDIA_SPECTRUM |
           METALIO_APP_CAP_DEVICE_INFO |
           METALIO_APP_CAP_DATE_TIME |
           METALIO_APP_CAP_UI_BUTTONS |
           METALIO_APP_CAP_UI_GRID |
           METALIO_APP_CAP_UI_DRAW |
           METALIO_APP_CAP_UI_SWIPE |
           METALIO_APP_CAP_HTTP |
           METALIO_APP_CAP_UI_LIST |
           METALIO_APP_CAP_UI_ICONS |
           METALIO_APP_CAP_AUDIO_RECORDING |
           METALIO_APP_CAP_UI_THEME |
           METALIO_APP_CAP_APP_STORAGE |
           METALIO_APP_CAP_UI_CONTROLS;
}

int ConfigWrite(void* host_context, const char* relative_path,
                const uint8_t* data, uint32_t data_size) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || !IsSafeRelativePath(relative_path)) {
        return METALIO_APP_STORAGE_ERROR_INVALID;
    }
    const std::string app_root =
        std::string(kAppDataRoot) + "/" + state->app.id;
    return WriteAppFile(app_root, relative_path, data, data_size);
}

int ConfigRead(void* host_context, const char* relative_path,
               uint8_t* buffer, uint32_t capacity, uint32_t* data_size) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || !IsSafeRelativePath(relative_path) ||
        data_size == nullptr) {
        return METALIO_APP_STORAGE_ERROR_INVALID;
    }
    const std::string app_root =
        std::string(kAppDataRoot) + "/" + state->app.id;
    const std::string data_path = app_root + "/" + relative_path;
    int result = ReadAppFile(data_path, buffer, capacity, data_size);
    if (result != METALIO_APP_STORAGE_ERROR_NOT_FOUND) return result;

    const std::string asset_path =
        state->app.root_path + "/assets/" + relative_path;
    result = ReadAppFile(asset_path, buffer, capacity, data_size);
    if (result != METALIO_APP_STORAGE_OK) return result;

    const int seed_result =
        WriteAppFile(app_root, relative_path, buffer, *data_size);
    if (seed_result != METALIO_APP_STORAGE_OK) {
        ESP_LOGW(kTag, "Could not seed %s config %s: %d",
                 state->app.id.c_str(), relative_path, seed_result);
    } else {
        ESP_LOGI(kTag, "Seeded %s config %s", state->app.id.c_str(),
                 relative_path);
    }
    return METALIO_APP_STORAGE_OK;
}

void FillTheme(metalio_app_theme_t* output) {
    if (output == nullptr) return;
    const Theme& theme = Theme::Get();
    const ThemeColors& colors = theme.colors();
    *output = {
        .dark = static_cast<uint8_t>(
            theme.appearance_mode() == AppearanceMode::Dark),
        .reserved = {},
        .background = colors.background,
        .surface = colors.surface,
        .raised = colors.raised,
        .border = colors.border,
        .text = colors.text,
        .muted = colors.muted,
        .accent = colors.accent,
        .accent_pressed = colors.accent_pressed,
        .accent_ink = colors.accent_ink,
        .danger = colors.danger,
        .warning = colors.warning,
    };
}

uint64_t ThemeSignature(const metalio_app_theme_t& theme) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&theme);
    uint64_t signature = UINT64_C(1469598103934665603);
    for (size_t index = 0; index < sizeof(theme); ++index) {
        signature ^= bytes[index];
        signature *= UINT64_C(1099511628211);
    }
    return signature;
}

void ApplySegmentTheme(ExternalSegment* segment,
                       const metalio_app_theme_t& theme);
void ApplyPickerTheme(ExternalPicker* picker,
                      const metalio_app_theme_t& theme);

void ApplyActionTheme(Runtime::State* state,
                      const metalio_app_theme_t& theme) {
    if (state == nullptr) return;
    if (state->actions != nullptr && lv_obj_is_valid(state->actions)) {
        lv_obj_set_style_bg_color(state->actions,
                                  lv_color_hex(theme.background),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_color(state->actions,
                                      lv_color_hex(theme.border), LV_PART_MAIN);
        lv_obj_t* back = lv_obj_get_child(state->actions, 0);
        if (back != nullptr) {
            lv_obj_set_style_bg_color(
                back, lv_color_hex(theme.raised),
                static_cast<lv_style_selector_t>(LV_PART_MAIN) |
                    static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
            const uint32_t child_count = lv_obj_get_child_count(back);
            for (uint32_t index = 0; index < child_count; ++index) {
                lv_obj_t* child =
                    lv_obj_get_child(back, static_cast<int32_t>(index));
                if (child != nullptr) {
                    lv_obj_set_style_text_color(child,
                                                lv_color_hex(theme.muted),
                                                LV_PART_MAIN);
                }
            }
        }
    }
    for (const auto& action : state->action_callbacks) {
        if (action == nullptr) continue;
        if (action->button != nullptr && lv_obj_is_valid(action->button)) {
            lv_obj_set_style_bg_color(
                action->button, lv_color_hex(theme.raised),
                static_cast<lv_style_selector_t>(LV_PART_MAIN) |
                    static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
        }
        if (action->icon != nullptr && lv_obj_is_valid(action->icon)) {
            lv_obj_set_style_text_color(action->icon,
                                        lv_color_hex(theme.muted), LV_PART_MAIN);
        }
        if (action->label != nullptr && lv_obj_is_valid(action->label)) {
            lv_obj_set_style_text_color(action->label,
                                        lv_color_hex(theme.muted), LV_PART_MAIN);
        }
    }
    for (const auto& segment : state->segments) {
        ApplySegmentTheme(segment.get(), theme);
    }
    for (const auto& picker : state->pickers) {
        ApplyPickerTheme(picker.get(), theme);
    }
}

void NotifyThemeIfChanged(Runtime::State* state, bool force = false) {
    if (state == nullptr) return;
    metalio_app_theme_t theme{};
    FillTheme(&theme);
    const uint64_t signature = ThemeSignature(theme);
    if (!force && signature == state->theme_signature) return;
    state->theme_signature = signature;
    ApplyActionTheme(state, theme);
    if (state->theme_callback != nullptr) {
        state->theme_callback(state->theme_app_context, &theme);
    }
}

void OnThemeTimer(lv_timer_t* timer) {
    auto* state = static_cast<Runtime::State*>(lv_timer_get_user_data(timer));
    if (state != nullptr && !state->paused) NotifyThemeIfChanged(state);
}

int GetTheme(void* host_context, metalio_app_theme_t* theme) {
    if (CheckedState(host_context) == nullptr || theme == nullptr) return -1;
    FillTheme(theme);
    return 0;
}

int SetThemeCallback(void* host_context,
                     metalio_app_theme_callback_t callback,
                     void* app_context) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    state->theme_callback = callback;
    state->theme_app_context = app_context;
    if (callback == nullptr) {
        if (state->theme_timer != nullptr) {
            lv_timer_delete(state->theme_timer);
            state->theme_timer = nullptr;
        }
        return 0;
    }
    if (state->theme_timer == nullptr) {
        state->theme_timer = lv_timer_create(OnThemeTimer, 250, state);
        if (state->theme_timer == nullptr) {
            state->theme_callback = nullptr;
            state->theme_app_context = nullptr;
            return -1;
        }
    }
    NotifyThemeIfChanged(state, true);
    return 0;
}

int PlayHostHaptic(void* host_context, metalio_app_haptic_effect_t effect) {
    if (CheckedState(host_context) == nullptr) return -1;
    switch (effect) {
        case METALIO_APP_HAPTIC_TICK:
            PlayHaptic(HapticStrength::Light);
            return 0;
        case METALIO_APP_HAPTIC_CLICK:
            PlayHaptic(HapticStrength::Medium);
            return 0;
        default:
            return -1;
    }
}

int GetMotionSample(void* host_context,
                    metalio_app_motion_sample_t* sample) {
    if (CheckedState(host_context) == nullptr || sample == nullptr) return -1;
    *sample = {};

    Sc7a20Sample acceleration;
    if (Sc7a20MotionService::GetInstance().ReadAcceleration(&acceleration)) {
        sample->acceleration_x_mg = acceleration.x_mg;
        sample->acceleration_y_mg = acceleration.y_mg;
        sample->acceleration_z_mg = acceleration.z_mg;
        sample->acceleration_valid = 1;
    }

    Sc7a20Tilt tilt;
    if (Sc7a20MotionService::GetInstance().ReadTilt(&tilt) && tilt.valid) {
        sample->tilt_x_q10 = tilt.x_q10;
        sample->tilt_y_q10 = tilt.y_q10;
        sample->tilt_valid = 1;
    }
    return 0;
}

int GetDeviceInfo(void* host_context, metalio_app_device_info_t* info) {
    if (CheckedState(host_context) == nullptr || info == nullptr) return -1;
    *info = {};
    CopyText(info->board_name, sizeof(info->board_name), BOARD_NAME);
    const std::string chip = SystemInfo::GetChipModelName();
    CopyText(info->chip_model, sizeof(info->chip_model), chip.c_str());
    const esp_app_desc_t* app = esp_app_get_description();
    CopyText(info->firmware_version, sizeof(info->firmware_version),
             app != nullptr ? app->version : "");
    info->flash_size_bytes = static_cast<uint32_t>(SystemInfo::GetFlashSize());
    info->free_internal_bytes = static_cast<uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    info->free_psram_bytes = static_cast<uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    info->uptime_seconds = static_cast<uint32_t>(
        esp_timer_get_time() / UINT64_C(1000000));
    int battery = 0;
    bool charging = false;
    bool discharging = false;
    if (Board::GetInstance().GetBatteryLevel(battery, charging, discharging)) {
        info->battery_percent =
            static_cast<int16_t>(std::clamp(battery, 0, 100));
        info->battery_valid = 1;
        info->charging = static_cast<uint8_t>(charging);
    }
    info->network_connected =
        static_cast<uint8_t>(Board::GetInstance().IsNetworkConnected());
    return 0;
}

int GetDateTime(void* host_context, metalio_app_date_time_t* output) {
    if (CheckedState(host_context) == nullptr || output == nullptr) return -1;
    *output = {};
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr || local.tm_year < 125) return 0;
    output->unix_seconds = static_cast<int64_t>(now);
    output->year = static_cast<int16_t>(local.tm_year + 1900);
    output->month = static_cast<int8_t>(local.tm_mon + 1);
    output->day = static_cast<int8_t>(local.tm_mday);
    output->hour = static_cast<int8_t>(local.tm_hour);
    output->minute = static_cast<int8_t>(local.tm_min);
    output->second = static_cast<int8_t>(local.tm_sec);
    output->weekday = static_cast<int8_t>(local.tm_wday);
    output->valid = 1;
    return 0;
}

int GetMagneticSample(void* host_context,
                      metalio_app_magnetic_sample_t* sample) {
    if (CheckedState(host_context) == nullptr || sample == nullptr) return -1;
    *sample = {};
    // MetalioClaw4 currently has no magnetometer. Keep the ABI callable so a
    // future board can advertise the capability without changing App code.
    return -2;
}

int AddBar(void* host_context, int16_t x, int16_t y, int16_t width,
           int16_t height, uint32_t track_rgb888,
           uint32_t indicator_rgb888, metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || widget == nullptr ||
        !IsValidRect(x, y, width, height)) {
        return -1;
    }
    lv_obj_t* bar = lv_bar_create(state->content);
    if (bar == nullptr) return -1;
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, width, height);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_bar_set_orientation(
        bar, height > width ? LV_BAR_ORIENTATION_VERTICAL
                            : LV_BAR_ORIENTATION_HORIZONTAL);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(track_rgb888 & 0xFFFFFFU),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        bar, lv_color_hex(indicator_rgb888 & 0xFFFFFFU), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    const metalio_app_widget_t id = state->next_widget_id++;
    state->bars.push_back({.id = id, .object = bar});
    *widget = id;
    return 0;
}

int SetBarValue(void* host_context, metalio_app_widget_t widget,
                uint16_t value_per_mille) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || value_per_mille > 1000) return -1;
    const auto found = std::find_if(
        state->bars.begin(), state->bars.end(),
        [widget](const ExternalBar& bar) { return bar.id == widget; });
    if (found == state->bars.end() || found->object == nullptr ||
        !lv_obj_is_valid(found->object)) {
        return -1;
    }
    lv_bar_set_value(found->object, value_per_mille, LV_ANIM_OFF);
    return 0;
}

int SetBarColors(void* host_context, metalio_app_widget_t widget,
                 uint32_t track_rgb888, uint32_t indicator_rgb888) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    const auto found = std::find_if(
        state->bars.begin(), state->bars.end(),
        [widget](const ExternalBar& bar) { return bar.id == widget; });
    if (found == state->bars.end() || found->object == nullptr ||
        !lv_obj_is_valid(found->object)) {
        return -1;
    }
    lv_obj_set_style_bg_color(found->object,
                              lv_color_hex(track_rgb888 & 0xFFFFFFU),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(found->object,
                              lv_color_hex(indicator_rgb888 & 0xFFFFFFU),
                              LV_PART_INDICATOR);
    return 0;
}

int MediaStart(void* host_context, const char* url) {
    Runtime::State* state = CheckedState(host_context);
    return state != nullptr ? MediaService::Get().Start(state, url) : -1;
}

int MediaPause(void* host_context) {
    Runtime::State* state = CheckedState(host_context);
    return state != nullptr ? MediaService::Get().Pause(state) : -1;
}

int MediaResume(void* host_context) {
    Runtime::State* state = CheckedState(host_context);
    return state != nullptr ? MediaService::Get().Resume(state) : -1;
}

int MediaStop(void* host_context) {
    Runtime::State* state = CheckedState(host_context);
    return state != nullptr ? MediaService::Get().Stop(state) : -1;
}

int MediaGetState(void* host_context, metalio_app_media_state_t* state_out) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || state_out == nullptr) return -1;
    switch (MediaService::Get().state(state)) {
        case MediaState::Connecting:
            *state_out = METALIO_APP_MEDIA_CONNECTING;
            break;
        case MediaState::Playing:
            *state_out = METALIO_APP_MEDIA_PLAYING;
            break;
        case MediaState::Paused:
            *state_out = METALIO_APP_MEDIA_PAUSED;
            break;
        case MediaState::Error:
            *state_out = METALIO_APP_MEDIA_ERROR;
            break;
        case MediaState::Idle:
        default:
            *state_out = METALIO_APP_MEDIA_IDLE;
            break;
    }
    return 0;
}

int MediaGetSpectrum(
    void* host_context,
    uint8_t levels[METALIO_APP_MEDIA_SPECTRUM_BANDS]) {
    Runtime::State* state = CheckedState(host_context);
    return state != nullptr
               ? MediaService::Get().GetSpectrum(
                     state, levels, METALIO_APP_MEDIA_SPECTRUM_BANDS)
               : -1;
}

int MediaGetVolume(void* host_context, uint8_t* volume_percent) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || volume_percent == nullptr) return -1;
    const int volume = MediaService::Get().GetVolume(state);
    if (volume < 0) return -1;
    *volume_percent = static_cast<uint8_t>(volume);
    return 0;
}

int MediaSetVolume(void* host_context, uint8_t volume_percent) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || volume_percent > 100) return -1;
    return MediaService::Get().SetVolume(state, volume_percent);
}

int RecordingStart(
        void* host_context, const metalio_app_recording_config_t* config) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return METALIO_APP_RECORDING_ERROR_INVALID;
    // Recording owns the external audio session; a Radio/Music App must stop
    // playback before the microphone route is activated.
    MediaService::Get().Stop(state);
    return RecordingService::Get().Start(state, config);
}

int RecordingStop(void* host_context) {
    Runtime::State* state = CheckedState(host_context);
    return state != nullptr ? RecordingService::Get().Stop(state)
                            : METALIO_APP_RECORDING_ERROR_INVALID;
}

int RecordingCancel(void* host_context) {
    Runtime::State* state = CheckedState(host_context);
    return state != nullptr ? RecordingService::Get().Cancel(state)
                            : METALIO_APP_RECORDING_ERROR_INVALID;
}

int RecordingGetStatus(
        void* host_context, metalio_app_recording_status_t* status) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || status == nullptr) {
        return METALIO_APP_RECORDING_ERROR_INVALID;
    }
    const int result = RecordingService::Get().GetStatus(state, status);
    if (result == METALIO_APP_RECORDING_ERROR_INVALID) {
        *status = {};
        status->state = METALIO_APP_RECORDING_IDLE;
        status->error = METALIO_APP_RECORDING_OK;
        return METALIO_APP_RECORDING_OK;
    }
    return result;
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
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
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

bool ResolveImageSource(Runtime::State* state, const char* asset_relative_path,
                        std::string* lv_path) {
    if (state == nullptr || asset_relative_path == nullptr || lv_path == nullptr ||
        !IsSafeRelativePath(asset_relative_path)) {
        return false;
    }
    const std::string posix_path = state->app.root_path + "/" + asset_relative_path;
    struct stat info {};
    if (!IsImageFile(posix_path) || stat(posix_path.c_str(), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<size_t>(info.st_size) > kMaxAssetBytes) {
        return false;
    }
    *lv_path = "S:" + posix_path;
    return true;
}

lv_obj_t* CreateImage(Runtime::State* state, const char* asset_relative_path,
                      int16_t x, int16_t y, int16_t width, int16_t height,
                      std::string** source) {
    if (state == nullptr || source == nullptr ||
        !IsValidRect(x, y, width, height)) {
        return nullptr;
    }
    std::string resolved;
    if (!ResolveImageSource(state, asset_relative_path, &resolved)) return nullptr;

    auto* lv_path = new (std::nothrow) std::string(std::move(resolved));
    if (lv_path == nullptr) return nullptr;
    lv_obj_t* image = lv_image_create(state->content);
    if (image == nullptr) {
        delete lv_path;
        return nullptr;
    }
    lv_obj_set_pos(image, x, y);
    lv_obj_set_size(image, width, height);
    lv_obj_add_flag(image, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(image, lv_path->c_str());
    lv_obj_add_event_cb(image, OnImageDeleted, LV_EVENT_DELETE, lv_path);
    *source = lv_path;
    return image;
}

int AddImage(void* host_context, const char* asset_relative_path, int16_t x,
             int16_t y, int16_t width, int16_t height) {
    Runtime::State* state = CheckedState(host_context);
    std::string* source = nullptr;
    return CreateImage(state, asset_relative_path, x, y, width, height,
                       &source) != nullptr
               ? 0
               : -1;
}

int AddImageEx(void* host_context, const char* asset_relative_path, int16_t x,
               int16_t y, int16_t width, int16_t height,
               metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || widget == nullptr) return -1;
    std::string* source = nullptr;
    lv_obj_t* image =
        CreateImage(state, asset_relative_path, x, y, width, height, &source);
    if (image == nullptr) return -1;
    const metalio_app_widget_t id = state->next_widget_id++;
    state->images.push_back({id, image, source});
    *widget = id;
    return 0;
}

int SetImageSource(void* host_context, metalio_app_widget_t widget,
                   const char* asset_relative_path) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    const auto found = std::find_if(
        state->images.begin(), state->images.end(),
        [widget](const ExternalImage& image) { return image.id == widget; });
    if (found == state->images.end() || found->object == nullptr ||
        found->source == nullptr) {
        return -1;
    }
    std::string resolved;
    if (!ResolveImageSource(state, asset_relative_path, &resolved)) return -1;
    lv_image_set_src(found->object, nullptr);
    *found->source = std::move(resolved);
    lv_image_set_src(found->object, found->source->c_str());
    lv_obj_invalidate(found->object);
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

int SetLabelColor(void* host_context, metalio_app_widget_t widget,
                  uint32_t rgb888) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    const auto found = std::find_if(
        state->labels.begin(), state->labels.end(),
        [widget](const ExternalLabel& label) { return label.id == widget; });
    if (found == state->labels.end() || found->object == nullptr ||
        !lv_obj_is_valid(found->object)) {
        return -1;
    }
    lv_obj_set_style_text_color(found->object,
                                lv_color_hex(rgb888 & 0xFFFFFFU),
                                LV_PART_MAIN);
    return 0;
}

int AddRect(void* host_context, int16_t x, int16_t y, int16_t width,
            int16_t height, uint32_t rgb888, uint16_t radius,
            metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || widget == nullptr ||
        !IsValidRect(x, y, width, height)) {
        return -1;
    }
    lv_obj_t* object = lv_obj_create(state->content);
    if (object == nullptr) return -1;
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(rgb888 & 0xFFFFFFU),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(object, std::min<uint16_t>(radius, 128),
                            LV_PART_MAIN);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
    const metalio_app_widget_t id = state->next_widget_id++;
    state->rectangles.push_back({.id = id, .object = object});
    *widget = id;
    return 0;
}

int SetRectColor(void* host_context, metalio_app_widget_t widget,
                 uint32_t rgb888) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    const auto found = std::find_if(
        state->rectangles.begin(), state->rectangles.end(),
        [widget](const ExternalRect& rectangle) {
            return rectangle.id == widget;
        });
    if (found == state->rectangles.end() || found->object == nullptr ||
        !lv_obj_is_valid(found->object)) {
        return -1;
    }
    lv_obj_set_style_bg_color(found->object,
                              lv_color_hex(rgb888 & 0xFFFFFFU),
                              LV_PART_MAIN);
    return 0;
}

void OnExternalButtonClicked(lv_event_t* event) {
    auto* button =
        static_cast<ExternalButton*>(lv_event_get_user_data(event));
    if (button != nullptr && button->callback != nullptr) {
        button->callback(button->app_context);
    }
}

ExternalButton* CreateExternalButton(
    Runtime::State* state, lv_obj_t* parent, const char* text, int16_t x,
    int16_t y, int16_t width, int16_t height, uint32_t background_rgb888,
    uint32_t text_rgb888, metalio_app_font_t font,
    metalio_app_callback_t callback, void* app_context,
    metalio_app_widget_t list_id = 0) {
    if (state == nullptr || parent == nullptr || text == nullptr || width <= 0 ||
        height <= 0 || strnlen(text, kMaxLabelBytes + 1) > kMaxLabelBytes) {
        return nullptr;
    }
    auto record = std::make_unique<ExternalButton>();
    record->id = state->next_widget_id++;
    record->list_id = list_id;
    record->callback = callback;
    record->app_context = app_context;
    record->button = lv_button_create(parent);
    if (record->button == nullptr) return nullptr;
    lv_obj_set_pos(record->button, x, y);
    lv_obj_set_size(record->button, width, height);
    lv_obj_set_style_bg_color(
        record->button, lv_color_hex(background_rgb888 & 0xFFFFFFU),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(record->button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(record->button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(record->button, metrics::kRadiusControl,
                            LV_PART_MAIN);
    lv_obj_add_flag(record->button, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (callback != nullptr) {
        lv_obj_add_event_cb(record->button, OnExternalButtonClicked,
                            LV_EVENT_CLICKED, record.get());
    }
    record->label = lv_label_create(record->button);
    if (record->label == nullptr) {
        lv_obj_delete(record->button);
        return nullptr;
    }
    lv_label_set_text(record->label, text);
    lv_label_set_long_mode(record->label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(record->label, std::max<int16_t>(1, width - 16));
    lv_obj_set_style_text_align(record->label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(record->label,
                                lv_color_hex(text_rgb888 & 0xFFFFFFU),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(record->label, ResolveFont(font), LV_PART_MAIN);
    lv_obj_center(record->label);
    ExternalButton* result = record.get();
    state->buttons.push_back(std::move(record));
    return result;
}

int AddButton(void* host_context, const char* text, int16_t x, int16_t y,
              int16_t width, int16_t height, uint32_t background_rgb888,
              uint32_t text_rgb888, metalio_app_font_t font,
              metalio_app_callback_t callback, void* app_context,
              metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || widget == nullptr ||
        !IsValidRect(x, y, width, height)) {
        return -1;
    }
    ExternalButton* button = CreateExternalButton(
        state, state->content, text, x, y, width, height,
        background_rgb888, text_rgb888, font, callback, app_context);
    if (button == nullptr) return -1;
    *widget = button->id;
    return 0;
}

ExternalButton* FindButton(Runtime::State* state,
                           metalio_app_widget_t widget) {
    if (state == nullptr) return nullptr;
    const auto found = std::find_if(
        state->buttons.begin(), state->buttons.end(),
        [widget](const std::unique_ptr<ExternalButton>& button) {
            return button != nullptr && button->id == widget;
        });
    return found != state->buttons.end() ? found->get() : nullptr;
}

int SetButtonText(void* host_context, metalio_app_widget_t widget,
                  const char* text) {
    Runtime::State* state = CheckedState(host_context);
    ExternalButton* button = FindButton(state, widget);
    if (button == nullptr || button->label == nullptr || text == nullptr ||
        strnlen(text, kMaxLabelBytes + 1) > kMaxLabelBytes ||
        !lv_obj_is_valid(button->label)) {
        return -1;
    }
    lv_label_set_text(button->label, text);
    return 0;
}

int SetButtonEnabled(void* host_context, metalio_app_widget_t widget,
                     uint8_t enabled) {
    Runtime::State* state = CheckedState(host_context);
    ExternalButton* button = FindButton(state, widget);
    if (button == nullptr || button->button == nullptr ||
        !lv_obj_is_valid(button->button)) {
        return -1;
    }
    if (enabled != 0) {
        lv_obj_remove_state(button->button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(button->button, LV_STATE_DISABLED);
    }
    return 0;
}

int SetButtonColors(void* host_context, metalio_app_widget_t widget,
                    uint32_t background_rgb888, uint32_t pressed_rgb888,
                    uint32_t text_rgb888) {
    Runtime::State* state = CheckedState(host_context);
    ExternalButton* button = FindButton(state, widget);
    if (button == nullptr || button->button == nullptr ||
        button->label == nullptr || !lv_obj_is_valid(button->button) ||
        !lv_obj_is_valid(button->label)) {
        return -1;
    }
    lv_obj_set_style_bg_color(button->button,
                              lv_color_hex(background_rgb888 & 0xFFFFFFU),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        button->button, lv_color_hex(pressed_rgb888 & 0xFFFFFFU),
        static_cast<lv_style_selector_t>(LV_PART_MAIN) |
            static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_set_style_text_color(button->label,
                                lv_color_hex(text_rgb888 & 0xFFFFFFU),
                                LV_PART_MAIN);
    return 0;
}

int AddGrid(void* host_context, int16_t x, int16_t y, int16_t width,
            int16_t height, uint8_t columns, uint8_t rows,
            uint8_t column_gap, uint8_t row_gap,
            metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || widget == nullptr ||
        !IsValidRect(x, y, width, height) || columns == 0 || rows == 0 ||
        columns > kMaximumGridAxis || rows > kMaximumGridAxis ||
        static_cast<int>(column_gap) * (columns - 1) >= width ||
        static_cast<int>(row_gap) * (rows - 1) >= height) {
        return -1;
    }
    lv_obj_t* object = lv_obj_create(state->content);
    if (object == nullptr) return -1;
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
    const metalio_app_widget_t id = state->next_widget_id++;
    state->grids.push_back({.id = id,
                            .object = object,
                            .width = width,
                            .height = height,
                            .columns = columns,
                            .rows = rows,
                            .column_gap = column_gap,
                            .row_gap = row_gap});
    *widget = id;
    return 0;
}

int GridAddButton(void* host_context, metalio_app_widget_t grid_widget,
                  uint8_t column, uint8_t row, uint8_t column_span,
                  uint8_t row_span, const char* text,
                  uint32_t background_rgb888, uint32_t text_rgb888,
                  metalio_app_font_t font, metalio_app_callback_t callback,
                  void* app_context, metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || widget == nullptr || column_span == 0 ||
        row_span == 0) {
        return -1;
    }
    const auto found = std::find_if(
        state->grids.begin(), state->grids.end(),
        [grid_widget](const ExternalGrid& grid) {
            return grid.id == grid_widget;
        });
    if (found == state->grids.end() || found->object == nullptr ||
        column + column_span > found->columns || row + row_span > found->rows) {
        return -1;
    }
    const int cell_width =
        (found->width - found->column_gap * (found->columns - 1)) /
        found->columns;
    const int cell_height =
        (found->height - found->row_gap * (found->rows - 1)) / found->rows;
    const int x = column * (cell_width + found->column_gap);
    const int y = row * (cell_height + found->row_gap);
    const int width = cell_width * column_span +
                      found->column_gap * (column_span - 1);
    const int height =
        cell_height * row_span + found->row_gap * (row_span - 1);
    ExternalButton* button = CreateExternalButton(
        state, found->object, text, static_cast<int16_t>(x),
        static_cast<int16_t>(y), static_cast<int16_t>(width),
        static_cast<int16_t>(height), background_rgb888, text_rgb888, font,
        callback, app_context);
    if (button == nullptr) return -1;
    *widget = button->id;
    return 0;
}

const char* ResolveAppIcon(metalio_app_icon_t icon) {
    switch (icon) {
        case METALIO_APP_ICON_CALENDAR:
            return FONT_AWESOME_CALENDAR;
        case METALIO_APP_ICON_CALCULATOR:
            return FONT_AWESOME_CALCULATOR;
        case METALIO_APP_ICON_GAME:
            return FONT_AWESOME_GAMEPAD;
        case METALIO_APP_ICON_WEATHER:
            return FONT_AWESOME_CLOUD_SUN;
        case METALIO_APP_ICON_LEVEL:
            return FONT_AWESOME_COMPASS;
        case METALIO_APP_ICON_MAGNETIC:
            return FONT_AWESOME_COMPASS;
        case METALIO_APP_ICON_HAPTIC:
            return FONT_AWESOME_BELL;
        case METALIO_APP_ICON_RADIO:
            return FONT_AWESOME_VOLUME_HIGH;
        case METALIO_APP_ICON_MUSIC:
            return FONT_AWESOME_MUSIC;
        case METALIO_APP_ICON_MICROPHONE:
            return FONT_AWESOME_MICROPHONE;
        case METALIO_APP_ICON_SETTINGS:
            return FONT_AWESOME_GEAR;
        case METALIO_APP_ICON_CHEVRON_RIGHT:
            return FONT_AWESOME_ARROW_RIGHT;
        case METALIO_APP_ICON_INFO:
        default:
            return FONT_AWESOME_CIRCLE_INFO;
    }
}

int AddIcon(void* host_context, metalio_app_icon_t icon, int16_t x, int16_t y,
            int16_t width, int16_t height, uint32_t rgb888,
            metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || widget == nullptr ||
        !IsValidRect(x, y, width, height)) {
        return -1;
    }
    lv_obj_t* object = lv_label_create(state->content);
    if (object == nullptr) return -1;
    lv_label_set_text(object, ResolveAppIcon(icon));
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_text_font(object,
                               height >= 48 ? fonts::IconLarge() : fonts::Icon(),
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(object, lv_color_hex(rgb888 & 0xFFFFFFU),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    const metalio_app_widget_t id = state->next_widget_id++;
    state->icons.push_back({.id = id, .object = object});
    *widget = id;
    return 0;
}

int SetIcon(void* host_context, metalio_app_widget_t widget,
            metalio_app_icon_t icon) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    const auto found = std::find_if(
        state->icons.begin(), state->icons.end(),
        [widget](const ExternalIcon& value) { return value.id == widget; });
    if (found == state->icons.end() || found->object == nullptr ||
        !lv_obj_is_valid(found->object)) {
        return -1;
    }
    lv_label_set_text(found->object, ResolveAppIcon(icon));
    return 0;
}

int SetIconColor(void* host_context, metalio_app_widget_t widget,
                 uint32_t rgb888) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    const auto found = std::find_if(
        state->icons.begin(), state->icons.end(),
        [widget](const ExternalIcon& icon) { return icon.id == widget; });
    if (found == state->icons.end() || found->object == nullptr ||
        !lv_obj_is_valid(found->object)) {
        return -1;
    }
    lv_obj_set_style_text_color(found->object,
                                lv_color_hex(rgb888 & 0xFFFFFFU),
                                LV_PART_MAIN);
    return 0;
}

lv_obj_t* FindWidgetObject(Runtime::State* state,
                           metalio_app_widget_t widget) {
    if (state == nullptr || widget == 0) return nullptr;
    for (const ExternalLabel& value : state->labels) {
        if (value.id == widget) return value.object;
    }
    for (const ExternalBar& value : state->bars) {
        if (value.id == widget) return value.object;
    }
    for (const ExternalRect& value : state->rectangles) {
        if (value.id == widget) return value.object;
    }
    if (ExternalButton* value = FindButton(state, widget); value != nullptr) {
        return value->button;
    }
    for (const ExternalGrid& value : state->grids) {
        if (value.id == widget) return value.object;
    }
    for (const ExternalIcon& value : state->icons) {
        if (value.id == widget) return value.object;
    }
    for (const ExternalList& value : state->lists) {
        if (value.id == widget) return value.object;
    }
    for (const auto& value : state->sliders) {
        if (value != nullptr && value->id == widget) return value->object;
    }
    for (const auto& value : state->segments) {
        if (value != nullptr && value->id == widget) return value->object;
    }
    for (const auto& value : state->pickers) {
        if (value != nullptr && value->id == widget) return value->object;
    }
    return nullptr;
}

int SetLabelAlignment(void* host_context, metalio_app_widget_t widget,
                      metalio_app_text_align_t alignment) {
    Runtime::State* state = CheckedState(host_context);
    lv_obj_t* object = FindWidgetObject(state, widget);
    if (object == nullptr || !lv_obj_is_valid(object)) return -1;
    lv_text_align_t lv_alignment = LV_TEXT_ALIGN_LEFT;
    switch (alignment) {
        case METALIO_APP_TEXT_ALIGN_CENTER:
            lv_alignment = LV_TEXT_ALIGN_CENTER;
            break;
        case METALIO_APP_TEXT_ALIGN_RIGHT:
            lv_alignment = LV_TEXT_ALIGN_RIGHT;
            break;
        case METALIO_APP_TEXT_ALIGN_LEFT:
            break;
        default:
            return -1;
    }
    lv_obj_set_style_text_align(object, lv_alignment, LV_PART_MAIN);
    return 0;
}

int SetLabelFont(void* host_context, metalio_app_widget_t widget,
                 metalio_app_font_t font) {
    Runtime::State* state = CheckedState(host_context);
    lv_obj_t* object = FindWidgetObject(state, widget);
    if (object == nullptr || !lv_obj_is_valid(object)) return -1;
    lv_obj_set_style_text_font(object, ResolveFont(font), LV_PART_MAIN);
    return 0;
}

int SetWidgetBounds(void* host_context, metalio_app_widget_t widget,
                    int16_t x, int16_t y, int16_t width, int16_t height) {
    Runtime::State* state = CheckedState(host_context);
    lv_obj_t* object = FindWidgetObject(state, widget);
    if (object == nullptr || !lv_obj_is_valid(object) ||
        !IsValidRect(x, y, width, height)) {
        return -1;
    }
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    return 0;
}

int SetWidgetVisible(void* host_context, metalio_app_widget_t widget,
                     uint8_t visible) {
    Runtime::State* state = CheckedState(host_context);
    lv_obj_t* object = FindWidgetObject(state, widget);
    if (object == nullptr || !lv_obj_is_valid(object)) return -1;
    if (visible != 0) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
    return 0;
}

int SetRectBorder(void* host_context, metalio_app_widget_t widget,
                  uint32_t rgb888, uint8_t width) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    const auto found = std::find_if(
        state->rectangles.begin(), state->rectangles.end(),
        [widget](const ExternalRect& value) { return value.id == widget; });
    if (found == state->rectangles.end() ||
        found->object == nullptr || !lv_obj_is_valid(found->object)) {
        return -1;
    }
    lv_obj_set_style_border_color(found->object,
                                  lv_color_hex(rgb888 & 0xFFFFFFU),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(found->object, width, LV_PART_MAIN);
    return 0;
}

int SetButtonBorder(void* host_context, metalio_app_widget_t widget,
                    uint32_t rgb888, uint8_t width, uint16_t radius) {
    Runtime::State* state = CheckedState(host_context);
    ExternalButton* button = FindButton(state, widget);
    if (button == nullptr || button->button == nullptr ||
        !lv_obj_is_valid(button->button)) {
        return -1;
    }
    lv_obj_set_style_border_color(button->button,
                                  lv_color_hex(rgb888 & 0xFFFFFFU),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(button->button, width, LV_PART_MAIN);
    lv_obj_set_style_radius(button->button, std::min<uint16_t>(radius, 128),
                            LV_PART_MAIN);
    return 0;
}

void GuardExternalSliderRelease(lv_event_t* event) {
    auto* slider =
        static_cast<ExternalSlider*>(lv_event_get_user_data(event));
    if (slider == nullptr || slider->object == nullptr) return;

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        slider->pointer_down = true;
        slider->last_pressed_value = lv_slider_get_value(slider->object);
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        if (slider->restoring) return;
        lv_indev_t* indev = lv_indev_active();
        const bool pointer_release =
            slider->pointer_down && indev != nullptr &&
            lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED;
        if (pointer_release) {
            slider->restoring = true;
            lv_slider_set_value(slider->object, slider->last_pressed_value,
                                LV_ANIM_OFF);
            slider->restoring = false;
            return;
        }
        slider->last_pressed_value = lv_slider_get_value(slider->object);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        slider->pointer_down = false;
        if (lv_slider_get_value(slider->object) !=
            slider->last_pressed_value) {
            slider->restoring = true;
            lv_slider_set_value(slider->object, slider->last_pressed_value,
                                LV_ANIM_OFF);
            slider->restoring = false;
        }
    }
}

void OnExternalSliderChanged(lv_event_t* event) {
    auto* slider =
        static_cast<ExternalSlider*>(lv_event_get_user_data(event));
    if (slider == nullptr || slider->updating || slider->restoring ||
        slider->callback == nullptr || slider->object == nullptr) {
        return;
    }
    slider->callback(slider->app_context,
                     lv_slider_get_value(slider->object));
}

int AddSlider(void* host_context, int16_t x, int16_t y, int16_t width,
              int16_t height, int32_t minimum, int32_t maximum, int32_t value,
              uint32_t track_rgb888, uint32_t indicator_rgb888,
              uint32_t knob_rgb888, metalio_app_value_callback_t callback,
              void* app_context, metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || widget == nullptr || minimum >= maximum ||
        value < minimum || value > maximum ||
        !IsValidRect(x, y, width, height)) {
        return -1;
    }
    auto slider = std::make_unique<ExternalSlider>();
    slider->id = state->next_widget_id++;
    slider->callback = callback;
    slider->app_context = app_context;
    slider->last_pressed_value = value;
    slider->object = lv_slider_create(state->content);
    if (slider->object == nullptr) return -1;
    lv_obj_set_pos(slider->object, x, y);
    lv_obj_set_size(slider->object, width, height);
    lv_slider_set_range(slider->object, minimum, maximum);
    lv_slider_set_value(slider->object, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider->object,
                              lv_color_hex(track_rgb888 & 0xFFFFFFU),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider->object,
                              lv_color_hex(indicator_rgb888 & 0xFFFFFFU),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider->object,
                              lv_color_hex(knob_rgb888 & 0xFFFFFFU),
                              LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider->object, 5, LV_PART_KNOB);
    IgnoreSwipeBack(slider->object, true);
    lv_obj_add_event_cb(slider->object, GuardExternalSliderRelease,
                        LV_EVENT_PRESSED, slider.get());
    lv_obj_add_event_cb(slider->object, GuardExternalSliderRelease,
                        LV_EVENT_VALUE_CHANGED, slider.get());
    lv_obj_add_event_cb(slider->object, GuardExternalSliderRelease,
                        LV_EVENT_RELEASED, slider.get());
    lv_obj_add_event_cb(slider->object, GuardExternalSliderRelease,
                        LV_EVENT_PRESS_LOST, slider.get());
    if (callback != nullptr) {
        lv_obj_add_event_cb(slider->object, OnExternalSliderChanged,
                            LV_EVENT_VALUE_CHANGED, slider.get());
    }
    *widget = slider->id;
    state->sliders.push_back(std::move(slider));
    return 0;
}

ExternalSlider* FindSlider(Runtime::State* state,
                           metalio_app_widget_t widget) {
    if (state == nullptr) return nullptr;
    const auto found = std::find_if(
        state->sliders.begin(), state->sliders.end(),
        [widget](const std::unique_ptr<ExternalSlider>& value) {
            return value != nullptr && value->id == widget;
        });
    return found == state->sliders.end() ? nullptr : found->get();
}

int SetSliderValue(void* host_context, metalio_app_widget_t widget,
                   int32_t value) {
    Runtime::State* state = CheckedState(host_context);
    ExternalSlider* slider = FindSlider(state, widget);
    if (slider == nullptr || slider->object == nullptr ||
        !lv_obj_is_valid(slider->object)) {
        return -1;
    }
    slider->updating = true;
    lv_slider_set_value(slider->object, value, LV_ANIM_OFF);
    slider->updating = false;
    return 0;
}

int SetSliderColors(void* host_context, metalio_app_widget_t widget,
                    uint32_t track_rgb888, uint32_t indicator_rgb888,
                    uint32_t knob_rgb888) {
    Runtime::State* state = CheckedState(host_context);
    ExternalSlider* slider = FindSlider(state, widget);
    if (slider == nullptr || slider->object == nullptr ||
        !lv_obj_is_valid(slider->object)) {
        return -1;
    }
    lv_obj_set_style_bg_color(slider->object,
                              lv_color_hex(track_rgb888 & 0xFFFFFFU),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider->object,
                              lv_color_hex(indicator_rgb888 & 0xFFFFFFU),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider->object,
                              lv_color_hex(knob_rgb888 & 0xFFFFFFU),
                              LV_PART_KNOB);
    return 0;
}

bool PrepareCustomActionBar(Runtime::State* state) {
    if (state == nullptr || state->actions == nullptr ||
        !lv_obj_is_valid(state->actions) ||
        lv_obj_get_child_count(state->actions) == 0) {
        return false;
    }
    lv_obj_set_layout(state->actions, LV_LAYOUT_NONE);
    lv_obj_t* back = lv_obj_get_child(state->actions, 0);
    if (back == nullptr) return false;
    lv_obj_set_pos(back, kActionBarLeft, kActionBarTop);
    lv_obj_set_size(back, 72, metrics::kBottomActionHeight);
    return true;
}

void ApplySegmentTheme(ExternalSegment* segment,
                       const metalio_app_theme_t& theme) {
    if (segment == nullptr || segment->object == nullptr ||
        !lv_obj_is_valid(segment->object)) {
        return;
    }
    lv_obj_set_style_bg_color(segment->object, lv_color_hex(theme.raised),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(segment->object, lv_color_hex(theme.border),
                                  LV_PART_MAIN);
    for (ExternalSegmentItem& item : segment->items) {
        if (item.button == nullptr || item.label == nullptr) continue;
        const bool selected = item.index == segment->selected;
        lv_obj_set_style_bg_color(
            item.button,
            lv_color_hex(selected ? theme.surface : theme.raised),
            LV_PART_MAIN);
        lv_obj_set_style_bg_opa(item.button,
                                selected ? LV_OPA_COVER : LV_OPA_TRANSP,
                                LV_PART_MAIN);
        lv_obj_set_style_bg_color(item.button,
                                  lv_color_hex(theme.accent_pressed),
                                  static_cast<lv_style_selector_t>(LV_PART_MAIN) |
                                      static_cast<lv_style_selector_t>(
                                          LV_STATE_PRESSED));
        lv_obj_set_style_text_color(
            item.label, lv_color_hex(selected ? theme.accent : theme.muted),
            LV_PART_MAIN);
    }
}

void SelectSegment(ExternalSegment* segment, uint8_t selected,
                   bool notify) {
    if (segment == nullptr || selected >= segment->items.size()) return;
    const bool changed = segment->selected != selected;
    segment->selected = selected;
    metalio_app_theme_t theme{};
    FillTheme(&theme);
    ApplySegmentTheme(segment, theme);
    if (notify && changed && segment->callback != nullptr) {
        segment->callback(segment->app_context, selected);
    }
}

void OnExternalSegmentClicked(lv_event_t* event) {
    auto* item =
        static_cast<ExternalSegmentItem*>(lv_event_get_user_data(event));
    if (item == nullptr || item->owner == nullptr) return;
    SelectSegment(item->owner, item->index, true);
}

int AddActionSegment(void* host_context, const char* const* labels,
                     uint8_t count, uint8_t selected,
                     metalio_app_selection_callback_t callback,
                     void* app_context, metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || labels == nullptr || widget == nullptr ||
        count < 2 || count > 4 || selected >= count ||
        !PrepareCustomActionBar(state)) {
        return -1;
    }
    auto segment = std::make_unique<ExternalSegment>();
    segment->id = state->next_widget_id++;
    segment->selected = selected;
    segment->callback = callback;
    segment->app_context = app_context;
    segment->items.reserve(count);
    segment->object = lv_obj_create(state->actions);
    if (segment->object == nullptr) return -1;
    lv_obj_remove_style_all(segment->object);
    lv_obj_set_pos(segment->object, 204, kActionBarTop);
    lv_obj_set_size(segment->object, 312, kPickerHeight);
    lv_obj_set_style_border_width(segment->object, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(segment->object, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(segment->object, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(segment->object, 4, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(segment->object, true, LV_PART_MAIN);
    lv_obj_set_flex_flow(segment->object, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(segment->object, LV_OBJ_FLAG_SCROLLABLE);
    IgnoreSwipeBack(segment->object, true);

    for (uint8_t index = 0; index < count; ++index) {
        if (labels[index] == nullptr ||
            strnlen(labels[index], kMaxLabelBytes + 1) > kMaxLabelBytes) {
            return -1;
        }
        segment->items.push_back({.owner = segment.get(), .index = index});
        ExternalSegmentItem& item = segment->items.back();
        item.button = lv_button_create(segment->object);
        if (item.button == nullptr) return -1;
        lv_obj_remove_style_all(item.button);
        lv_obj_set_height(item.button, 72);
        lv_obj_set_flex_grow(item.button, 1);
        lv_obj_set_style_radius(item.button, 11, LV_PART_MAIN);
        lv_obj_add_event_cb(item.button, OnExternalSegmentClicked,
                            LV_EVENT_CLICKED, &item);
        item.label = lv_label_create(item.button);
        if (item.label == nullptr) return -1;
        lv_label_set_text(item.label, labels[index]);
        lv_obj_set_style_text_font(item.label, fonts::SmallBold(),
                                   LV_PART_MAIN);
        lv_obj_center(item.label);
    }
    metalio_app_theme_t theme{};
    FillTheme(&theme);
    ApplySegmentTheme(segment.get(), theme);
    *widget = segment->id;
    state->segments.push_back(std::move(segment));
    return 0;
}

int SetActionSegmentSelected(void* host_context,
                             metalio_app_widget_t widget,
                             uint8_t selected) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    const auto found = std::find_if(
        state->segments.begin(), state->segments.end(),
        [widget](const std::unique_ptr<ExternalSegment>& value) {
            return value != nullptr && value->id == widget;
        });
    if (found == state->segments.end() || selected >= (*found)->items.size()) {
        return -1;
    }
    SelectSegment(found->get(), selected, false);
    return 0;
}

uint32_t WrapPickerIndex(const ExternalPicker* picker, int64_t index) {
    if (picker == nullptr || picker->labels.empty()) return 0;
    const int64_t count = static_cast<int64_t>(picker->labels.size());
    return static_cast<uint32_t>((index % count + count) % count);
}

void ApplyPickerTheme(ExternalPicker* picker,
                      const metalio_app_theme_t& theme) {
    if (picker == nullptr || picker->object == nullptr ||
        !lv_obj_is_valid(picker->object)) {
        return;
    }
    lv_obj_set_style_bg_color(picker->object, lv_color_hex(theme.background),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(picker->object, lv_color_hex(theme.border),
                                  LV_PART_MAIN);
    for (int offset = -2; offset <= 2; ++offset) {
        const size_t slot = static_cast<size_t>(offset + 2);
        const bool selected = offset == 0;
        if (picker->items[slot] != nullptr) {
            lv_obj_set_style_border_color(
                picker->items[slot],
                lv_color_hex(selected ? theme.accent : theme.background),
                LV_PART_MAIN);
            lv_obj_set_style_opa(picker->items[slot],
                                 selected ? LV_OPA_COVER : LV_OPA_60,
                                 LV_PART_MAIN);
        }
        if (picker->numbers[slot] != nullptr) {
            lv_obj_set_style_text_color(
                picker->numbers[slot],
                lv_color_hex(selected ? theme.accent : theme.muted),
                LV_PART_MAIN);
        }
        if (picker->names[slot] != nullptr) {
            lv_obj_set_style_text_color(
                picker->names[slot],
                lv_color_hex(selected ? theme.text : theme.muted),
                LV_PART_MAIN);
        }
    }
}

void ApplyPickerGeometry(ExternalPicker* picker) {
    if (picker == nullptr || picker->labels.empty()) return;
    for (int offset = -2; offset <= 2; ++offset) {
        const size_t slot = static_cast<size_t>(offset + 2);
        const uint32_t station = WrapPickerIndex(
            picker, static_cast<int64_t>(picker->selected) + offset);
        const int center_x = kPickerWidth / 2 + offset * kPickerStep +
                             static_cast<int>(std::lround(picker->offset));
        lv_obj_set_pos(picker->items[slot], center_x - kPickerStep / 2, 0);
        lv_label_set_text_fmt(picker->numbers[slot], "%u",
                              static_cast<unsigned>(station + 1));
        lv_label_set_text(picker->names[slot],
                          picker->labels[station].c_str());
    }
}

void StopPickerMotion(ExternalPicker* picker) {
    if (picker == nullptr || picker->motion_timer == nullptr) return;
    lv_timer_t* timer = picker->motion_timer;
    picker->motion_timer = nullptr;
    lv_timer_delete(timer);
}

void NotifyPickerSelection(ExternalPicker* picker) {
    if (picker == nullptr || picker->selected == picker->notified) return;
    picker->notified = picker->selected;
    if (picker->callback != nullptr) {
        picker->callback(picker->app_context, picker->selected);
    }
}

void FinishPickerMotion(ExternalPicker* picker) {
    if (picker == nullptr) return;
    StopPickerMotion(picker);
    picker->velocity = 0.0f;
    picker->offset = 0.0f;
    picker->position = picker->snap_target_position;
    picker->snap_elapsed_ms = 0.0f;
    picker->snapping = false;
    ApplyPickerGeometry(picker);
    NotifyPickerSelection(picker);
}

void BeginPickerSnap(ExternalPicker* picker) {
    if (picker == nullptr) return;
    picker->velocity = 0.0f;
    picker->snap_start_offset = picker->offset;
    picker->snap_target_position = std::round(picker->position);
    picker->snap_elapsed_ms = 0.0f;
    picker->snapping = true;
    if (std::abs(picker->snap_start_offset) < 0.5f) {
        FinishPickerMotion(picker);
    }
}

void AdvancePicker(ExternalPicker* picker, float delta) {
    if (picker == nullptr) return;
    picker->position -= delta / static_cast<float>(kPickerStep);
    picker->offset += delta;
    while (picker->offset <= -kPickerStep / 2.0f) {
        picker->offset += kPickerStep;
        picker->selected = WrapPickerIndex(
            picker, static_cast<int64_t>(picker->selected) + 1);
        PlayHaptic(HapticStrength::Light);
    }
    while (picker->offset >= kPickerStep / 2.0f) {
        picker->offset -= kPickerStep;
        picker->selected = WrapPickerIndex(
            picker, static_cast<int64_t>(picker->selected) - 1);
        PlayHaptic(HapticStrength::Light);
    }
    ApplyPickerGeometry(picker);
}

void OnPickerMotion(lv_timer_t* timer) {
    auto* picker = timer != nullptr
                       ? static_cast<ExternalPicker*>(
                             lv_timer_get_user_data(timer))
                       : nullptr;
    if (picker == nullptr || picker->motion_timer != timer) return;
    const int64_t now_us = esp_timer_get_time();
    float elapsed_ms =
        static_cast<float>(now_us - picker->motion_last_us) / 1000.0f;
    picker->motion_last_us = now_us;
    elapsed_ms = std::clamp(elapsed_ms, 1.0f, 32.0f);
    if (picker->snapping) {
        picker->snap_elapsed_ms += elapsed_ms;
        const float t =
            std::min(1.0f, picker->snap_elapsed_ms / kPickerSnapDurationMs);
        const float spring = (1.0f - t) * (1.0f - 1.35f * t);
        picker->offset = picker->snap_start_offset * spring;
        picker->position = picker->snap_target_position -
                           picker->offset / static_cast<float>(kPickerStep);
        ApplyPickerGeometry(picker);
        if (t >= 1.0f) FinishPickerMotion(picker);
        return;
    }
    AdvancePicker(picker, picker->velocity * elapsed_ms);
    picker->velocity *=
        std::pow(kPickerFriction, elapsed_ms / 16.6667f);
    if (std::abs(picker->velocity) < kPickerStopVelocity) {
        BeginPickerSnap(picker);
    }
}

void StartPickerMotion(ExternalPicker* picker, float velocity) {
    if (picker == nullptr) return;
    StopPickerMotion(picker);
    picker->velocity =
        std::clamp(velocity, -kPickerMaxVelocity, kPickerMaxVelocity);
    picker->snapping = false;
    if (std::abs(picker->velocity) < kPickerMinReleaseVelocity) {
        BeginPickerSnap(picker);
    }
    if (picker->motion_timer == nullptr &&
        (picker->snapping ||
         std::abs(picker->velocity) >= kPickerMinReleaseVelocity)) {
        picker->motion_last_us = esp_timer_get_time();
        picker->motion_timer = lv_timer_create(
            OnPickerMotion, kPickerMotionPeriodMs, picker);
    }
}

void OnPickerPressed(lv_event_t* event) {
    auto* picker =
        static_cast<ExternalPicker*>(lv_event_get_user_data(event));
    if (picker == nullptr) return;
    StopPickerMotion(picker);
    lv_indev_t* indev = lv_event_get_indev(event);
    if (indev == nullptr) indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    picker->pointer_start_x = point.x;
    picker->pointer_last_x = point.x;
    picker->pointer_last_us = esp_timer_get_time();
    picker->velocity = 0.0f;
    picker->pointer_active = true;
    picker->pointer_moved = false;
}

void OnPickerPressing(lv_event_t* event) {
    auto* picker =
        static_cast<ExternalPicker*>(lv_event_get_user_data(event));
    if (picker == nullptr || !picker->pointer_active) return;
    lv_indev_t* indev = lv_event_get_indev(event);
    if (indev == nullptr) indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const int64_t now_us = esp_timer_get_time();
    const int delta_x = point.x - picker->pointer_last_x;
    const float elapsed_ms = std::max(
        1.0f, static_cast<float>(now_us - picker->pointer_last_us) / 1000.0f);
    picker->pointer_last_x = point.x;
    picker->pointer_last_us = now_us;
    if (std::abs(point.x - picker->pointer_start_x) >=
        kPickerDragThreshold) {
        picker->pointer_moved = true;
    }
    if (!picker->pointer_moved) return;
    if (delta_x == 0) {
        picker->velocity *= 0.72f;
        return;
    }
    const float instantaneous = delta_x / elapsed_ms;
    picker->velocity = picker->velocity * 0.68f + instantaneous * 0.32f;
    AdvancePicker(picker, static_cast<float>(delta_x));
}

void OnPickerReleased(lv_event_t* event) {
    auto* picker =
        static_cast<ExternalPicker*>(lv_event_get_user_data(event));
    if (picker == nullptr || !picker->pointer_active) return;
    picker->pointer_active = false;
    if (picker->pointer_moved) {
        StartPickerMotion(picker, picker->velocity * kPickerReleaseBoost);
        return;
    }
    const int picker_center = kPickerX + kPickerWidth / 2;
    const int direction = std::clamp(
        static_cast<int>(std::lround(
            static_cast<float>(picker->pointer_start_x - picker_center) /
            kPickerStep)),
        -2, 2);
    if (direction != 0) {
        picker->selected = WrapPickerIndex(
            picker, static_cast<int64_t>(picker->selected) + direction);
        picker->position += direction;
        picker->offset = 0.0f;
        ApplyPickerGeometry(picker);
        PlayHaptic(HapticStrength::Light);
        NotifyPickerSelection(picker);
    } else {
        StartPickerMotion(picker, 0.0f);
    }
}

int AddActionPicker(void* host_context, const char* const* labels,
                    uint32_t count, uint32_t selected,
                    metalio_app_selection_callback_t callback,
                    void* app_context, metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || labels == nullptr || widget == nullptr ||
        count == 0 || count > kMaximumControlOptions || selected >= count ||
        !PrepareCustomActionBar(state)) {
        return -1;
    }
    auto picker = std::make_unique<ExternalPicker>();
    picker->id = state->next_widget_id++;
    picker->selected = selected;
    picker->notified = selected;
    picker->callback = callback;
    picker->app_context = app_context;
    picker->labels.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        if (labels[index] == nullptr ||
            strnlen(labels[index], kMaxLabelBytes + 1) > kMaxLabelBytes) {
            return -1;
        }
        picker->labels.emplace_back(labels[index]);
    }
    picker->object = lv_obj_create(state->actions);
    if (picker->object == nullptr) return -1;
    lv_obj_remove_style_all(picker->object);
    lv_obj_set_pos(picker->object, kPickerX, kActionBarTop);
    lv_obj_set_size(picker->object, kPickerWidth, kPickerHeight);
    lv_obj_set_style_bg_opa(picker->object, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(picker->object, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(picker->object, LV_BORDER_SIDE_BOTTOM,
                                 LV_PART_MAIN);
    lv_obj_set_style_clip_corner(picker->object, true, LV_PART_MAIN);
    lv_obj_remove_flag(picker->object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(picker->object, LV_OBJ_FLAG_CLICKABLE);
    IgnoreSwipeBack(picker->object, true);
    lv_obj_add_event_cb(picker->object, OnPickerPressed, LV_EVENT_PRESSED,
                        picker.get());
    lv_obj_add_event_cb(picker->object, OnPickerPressing, LV_EVENT_PRESSING,
                        picker.get());
    lv_obj_add_event_cb(picker->object, OnPickerReleased, LV_EVENT_RELEASED,
                        picker.get());
    lv_obj_add_event_cb(picker->object, OnPickerReleased, LV_EVENT_PRESS_LOST,
                        picker.get());

    for (size_t slot = 0; slot < picker->items.size(); ++slot) {
        picker->items[slot] = lv_obj_create(picker->object);
        if (picker->items[slot] == nullptr) return -1;
        lv_obj_remove_style_all(picker->items[slot]);
        lv_obj_set_size(picker->items[slot], kPickerStep, 80);
        lv_obj_set_style_border_width(picker->items[slot], 3, LV_PART_MAIN);
        lv_obj_set_style_border_side(picker->items[slot],
                                     LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_remove_flag(picker->items[slot], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(picker->items[slot], LV_OBJ_FLAG_SCROLLABLE);

        picker->numbers[slot] = lv_label_create(picker->items[slot]);
        picker->names[slot] = lv_label_create(picker->items[slot]);
        if (picker->numbers[slot] == nullptr || picker->names[slot] == nullptr) {
            return -1;
        }
        lv_obj_set_style_text_font(picker->numbers[slot], fonts::SmallBold(),
                                   LV_PART_MAIN);
        lv_obj_set_style_text_align(picker->numbers[slot],
                                    LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_size(picker->numbers[slot], kPickerStep - 20, 28);
        lv_obj_set_pos(picker->numbers[slot], 10, 4);
        lv_label_set_long_mode(picker->numbers[slot], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(picker->names[slot], fonts::SmallBold(),
                                   LV_PART_MAIN);
        lv_obj_set_style_text_align(picker->names[slot], LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_set_size(picker->names[slot], kPickerStep - 20, 42);
        lv_obj_set_pos(picker->names[slot], 10, 34);
        lv_label_set_long_mode(picker->names[slot], LV_LABEL_LONG_DOT);
    }
    metalio_app_theme_t theme{};
    FillTheme(&theme);
    ApplyPickerTheme(picker.get(), theme);
    ApplyPickerGeometry(picker.get());
    *widget = picker->id;
    state->pickers.push_back(std::move(picker));
    return 0;
}

int SetActionPickerSelected(void* host_context,
                            metalio_app_widget_t widget,
                            uint32_t selected) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    const auto found = std::find_if(
        state->pickers.begin(), state->pickers.end(),
        [widget](const std::unique_ptr<ExternalPicker>& value) {
            return value != nullptr && value->id == widget;
        });
    if (found == state->pickers.end() || selected >= (*found)->labels.size()) {
        return -1;
    }
    ExternalPicker* picker = found->get();
    StopPickerMotion(picker);
    picker->selected = selected;
    picker->notified = selected;
    picker->offset = 0.0f;
    picker->position = static_cast<float>(selected);
    ApplyPickerGeometry(picker);
    return 0;
}

int AddList(void* host_context, int16_t x, int16_t y, int16_t width,
            int16_t height, metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || widget == nullptr ||
        !IsValidRect(x, y, width, height)) {
        return -1;
    }
    lv_obj_t* object = lv_obj_create(state->content);
    if (object == nullptr) return -1;
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(object, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(object, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(object, LV_DIR_VER);
    lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
    const metalio_app_widget_t id = state->next_widget_id++;
    state->lists.push_back({.id = id, .object = object});
    *widget = id;
    return 0;
}

int ListAddItem(void* host_context, metalio_app_widget_t list_widget,
                metalio_app_icon_t icon, const char* primary_text,
                const char* secondary_text, metalio_app_callback_t callback,
                void* app_context, metalio_app_widget_t* widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr || primary_text == nullptr || widget == nullptr ||
        strnlen(primary_text, kMaxLabelBytes + 1) > kMaxLabelBytes ||
        (secondary_text != nullptr &&
         strnlen(secondary_text, kMaxLabelBytes + 1) > kMaxLabelBytes)) {
        return -1;
    }
    const auto list = std::find_if(
        state->lists.begin(), state->lists.end(),
        [list_widget](const ExternalList& value) {
            return value.id == list_widget;
        });
    if (list == state->lists.end() || list->object == nullptr ||
        !lv_obj_is_valid(list->object)) {
        return -1;
    }

    auto record = std::make_unique<ExternalButton>();
    record->id = state->next_widget_id++;
    record->list_id = list_widget;
    record->callback = callback;
    record->app_context = app_context;
    record->button = lv_button_create(list->object);
    if (record->button == nullptr) return -1;
    lv_obj_set_width(record->button, LV_PCT(100));
    lv_obj_set_height(record->button, 78);
    lv_obj_set_flex_grow(record->button, 0);
    lv_obj_set_style_bg_color(record->button, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(record->button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(record->button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(record->button, lv_color_hex(0xE1E7F0),
                                  LV_PART_MAIN);
    lv_obj_set_style_radius(record->button, metrics::kRadiusControl,
                            LV_PART_MAIN);
    lv_obj_add_flag(record->button, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (callback != nullptr) {
        lv_obj_add_event_cb(record->button, OnExternalButtonClicked,
                            LV_EVENT_CLICKED, record.get());
    }
    lv_obj_t* icon_label = lv_label_create(record->button);
    lv_label_set_text(icon_label, ResolveAppIcon(icon));
    lv_obj_set_style_text_font(icon_label, fonts::Icon(), LV_PART_MAIN);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(0x52647E),
                                LV_PART_MAIN);
    lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 14, 0);

    record->label = lv_label_create(record->button);
    lv_label_set_text(record->label, primary_text);
    lv_obj_set_width(record->label, LV_PCT(70));
    lv_label_set_long_mode(record->label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(record->label, fonts::SmallBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(record->label, lv_color_hex(0x17223B),
                                LV_PART_MAIN);
    lv_obj_align(record->label, LV_ALIGN_TOP_LEFT, 52, 11);
    if (secondary_text != nullptr && secondary_text[0] != '\0') {
        lv_obj_t* secondary = lv_label_create(record->button);
        lv_label_set_text(secondary, secondary_text);
        lv_obj_set_width(secondary, LV_PCT(70));
        lv_label_set_long_mode(secondary, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(secondary, fonts::Small(), LV_PART_MAIN);
        lv_obj_set_style_text_color(secondary, lv_color_hex(0x687386),
                                    LV_PART_MAIN);
        lv_obj_align(secondary, LV_ALIGN_BOTTOM_LEFT, 52, -10);
    }
    lv_obj_t* chevron = lv_label_create(record->button);
    lv_label_set_text(chevron, FONT_AWESOME_ARROW_RIGHT);
    lv_obj_set_style_text_font(chevron, fonts::Icon(), LV_PART_MAIN);
    lv_obj_set_style_text_color(chevron, lv_color_hex(0x8791A2), LV_PART_MAIN);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -14, 0);

    *widget = record->id;
    state->buttons.push_back(std::move(record));
    return 0;
}

int ListClear(void* host_context, metalio_app_widget_t list_widget) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    const auto list = std::find_if(
        state->lists.begin(), state->lists.end(),
        [list_widget](const ExternalList& value) {
            return value.id == list_widget;
        });
    if (list == state->lists.end() || list->object == nullptr ||
        !lv_obj_is_valid(list->object)) {
        return -1;
    }
    for (const auto& button : state->buttons) {
        if (button == nullptr || button->list_id != list_widget ||
            button->button == nullptr || !lv_obj_is_valid(button->button)) {
            continue;
        }
        lv_obj_remove_event_cb_with_user_data(
            button->button, OnExternalButtonClicked, button.get());
    }
    state->buttons.erase(
        std::remove_if(
            state->buttons.begin(), state->buttons.end(),
            [list_widget](const std::unique_ptr<ExternalButton>& button) {
                return button != nullptr && button->list_id == list_widget;
            }),
        state->buttons.end());
    lv_obj_clean(list->object);
    return 0;
}

void OnContentSwipe(lv_event_t* event) {
    auto* state = static_cast<Runtime::State*>(lv_event_get_user_data(event));
    if (state == nullptr || state->swipe_callback == nullptr || state->paused) {
        return;
    }
    lv_indev_t* input = lv_indev_active();
    if (input == nullptr) return;
    metalio_app_swipe_direction_t direction;
    switch (lv_indev_get_gesture_dir(input)) {
        case LV_DIR_LEFT:
            direction = METALIO_APP_SWIPE_LEFT;
            break;
        case LV_DIR_RIGHT:
            direction = METALIO_APP_SWIPE_RIGHT;
            break;
        case LV_DIR_TOP:
            direction = METALIO_APP_SWIPE_UP;
            break;
        case LV_DIR_BOTTOM:
            direction = METALIO_APP_SWIPE_DOWN;
            break;
        default:
            return;
    }
    lv_event_stop_bubbling(event);
    lv_indev_wait_release(input);
    state->swipe_callback(state->swipe_app_context, direction);
}

int SetSwipeHandler(void* host_context,
                    metalio_app_swipe_callback_t callback,
                    void* app_context) {
    Runtime::State* state = CheckedState(host_context);
    if (state == nullptr) return -1;
    state->swipe_callback = callback;
    state->swipe_app_context = app_context;
    if (callback != nullptr && !state->swipe_event_attached) {
        lv_obj_add_event_cb(state->content, OnContentSwipe, LV_EVENT_GESTURE,
                            state);
        state->swipe_event_attached = true;
    } else if (callback == nullptr && state->swipe_event_attached) {
        lv_obj_remove_event_cb_with_user_data(state->content, OnContentSwipe,
                                              state);
        state->swipe_event_attached = false;
    }
    return 0;
}

int HttpRequest(void* host_context,
                const metalio_app_http_request_t* request,
                metalio_app_http_callback_t callback, void* app_context,
                uint32_t* request_id) {
    Runtime::State* state = CheckedState(host_context);
    return state != nullptr
               ? HttpService::Get().Start(state, request, callback,
                                          app_context, request_id)
               : -1;
}

int HttpCancel(void* host_context, uint32_t request_id) {
    Runtime::State* state = CheckedState(host_context);
    return state != nullptr ? HttpService::Get().Cancel(state, request_id)
                            : -1;
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
        case METALIO_APP_ACTION_PREVIOUS:
            return FONT_AWESOME_BACKWARD_STEP;
        case METALIO_APP_ACTION_PAUSE:
            return FONT_AWESOME_PAUSE;
        case METALIO_APP_ACTION_NEXT:
            return FONT_AWESOME_FORWARD_STEP;
        case METALIO_APP_ACTION_VOLUME_DOWN:
            return FONT_AWESOME_VOLUME_LOW;
        case METALIO_APP_ACTION_VOLUME_UP:
            return FONT_AWESOME_VOLUME_HIGH;
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
    action->icon = parts.icon;
    action->label = parts.label;
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

uint32_t RemainingResetTimeMs(int64_t deadline_us) {
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) return 0;
    return static_cast<uint32_t>((remaining_us + 999) / 1000);
}

bool ResetExternalServicesForLaunch(std::string* error) {
    const int64_t started_us = esp_timer_get_time();
    const int64_t deadline_us =
        started_us + static_cast<int64_t>(kExternalServiceResetTimeoutMs) * 1000;

    MediaService* media = MediaService::Existing();
    if (media != nullptr &&
        !media->ResetForAppLaunch(RemainingResetTimeMs(deadline_us))) {
        if (error != nullptr) *error = "媒体服务清理超时，请稍后重试";
        return false;
    }
    RecordingService* recording = RecordingService::Existing();
    if (recording != nullptr &&
        !recording->ResetForAppLaunch(RemainingResetTimeMs(deadline_us))) {
        if (error != nullptr) *error = "录音服务清理超时，请稍后重试";
        return false;
    }
    HttpService* http = HttpService::Existing();
    if (http != nullptr &&
        !http->ResetForAppLaunch(RemainingResetTimeMs(deadline_us))) {
        if (error != nullptr) *error = "网络服务清理超时，请稍后重试";
        return false;
    }
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE(kTag, "heap integrity failed after external service reset");
        if (error != nullptr) *error = "外部服务清理后的内存完整性检查失败";
        return false;
    }

    // Self-deleting FreeRTOS workers release their task allocation from the
    // idle task. Yield once after all service completion signals so ELF
    // relocation does not race that final allocator cleanup.
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(kTag,
             "External services reset before launch in %lld ms "
             "(internal=%u, largest=%u, psram=%u)",
             (esp_timer_get_time() - started_us) / 1000,
             static_cast<unsigned>(
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return true;
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
    if (!ResetExternalServicesForLaunch(error)) return false;
    if (!EnsureExternalAppSymbolsRegistered()) {
        if (error != nullptr) *error = "外部 App 符号 ABI 不可用";
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
        .get_capabilities = GetCapabilities,
        .play_haptic = PlayHostHaptic,
        .get_motion_sample = GetMotionSample,
        .add_bar = AddBar,
        .set_bar_value = SetBarValue,
        .media_start = MediaStart,
        .media_pause = MediaPause,
        .media_resume = MediaResume,
        .media_stop = MediaStop,
        .media_get_state = MediaGetState,
        .media_get_spectrum = MediaGetSpectrum,
        .media_get_volume = MediaGetVolume,
        .media_set_volume = MediaSetVolume,
        .get_device_info = GetDeviceInfo,
        .get_date_time = GetDateTime,
        .get_magnetic_sample = GetMagneticSample,
        .add_rect = AddRect,
        .set_rect_color = SetRectColor,
        .set_label_color = SetLabelColor,
        .add_button = AddButton,
        .set_button_text = SetButtonText,
        .set_button_enabled = SetButtonEnabled,
        .add_grid = AddGrid,
        .grid_add_button = GridAddButton,
        .set_swipe_handler = SetSwipeHandler,
        .add_icon = AddIcon,
        .set_icon = SetIcon,
        .add_list = AddList,
        .list_add_item = ListAddItem,
        .list_clear = ListClear,
        .http_request = HttpRequest,
        .http_cancel = HttpCancel,
        .recording_start = RecordingStart,
        .recording_stop = RecordingStop,
        .recording_cancel = RecordingCancel,
        .recording_get_status = RecordingGetStatus,
        .get_theme = GetTheme,
        .set_theme_callback = SetThemeCallback,
        .set_bar_colors = SetBarColors,
        .set_button_colors = SetButtonColors,
        .set_icon_color = SetIconColor,
        .config_read = ConfigRead,
        .config_write = ConfigWrite,
        .set_label_alignment = SetLabelAlignment,
        .set_label_font = SetLabelFont,
        .set_widget_bounds = SetWidgetBounds,
        .set_widget_visible = SetWidgetVisible,
        .set_rect_border = SetRectBorder,
        .set_button_border = SetButtonBorder,
        .add_slider = AddSlider,
        .set_slider_value = SetSliderValue,
        .set_slider_colors = SetSliderColors,
        .add_action_segment = AddActionSegment,
        .set_action_segment_selected = SetActionSegmentSelected,
        .add_action_picker = AddActionPicker,
        .set_action_picker_selected = SetActionPickerSelected,
        .add_image_ex = AddImageEx,
        .set_image_source = SetImageSource,
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
    if (paused) {
        if (MediaService* media = MediaService::Existing(); media != nullptr) {
            media->SuspendOwner(state_);
        }
        if (RecordingService* recording = RecordingService::Existing();
            recording != nullptr) {
            recording->SuspendOwner(state_);
        }
        if (state_->theme_timer != nullptr) lv_timer_pause(state_->theme_timer);
    } else {
        if (MediaService* media = MediaService::Existing(); media != nullptr) {
            media->ResumeOwner(state_);
        }
        NotifyThemeIfChanged(state_);
        if (state_->theme_timer != nullptr) lv_timer_resume(state_->theme_timer);
    }
    for (const auto& interval : state_->intervals) {
        if (interval == nullptr || interval->timer == nullptr) continue;
        if (paused) {
            lv_timer_pause(interval->timer);
        } else {
            lv_timer_resume(interval->timer);
        }
    }
    for (const auto& picker : state_->pickers) {
        if (picker == nullptr || picker->motion_timer == nullptr) continue;
        if (paused) {
            lv_timer_pause(picker->motion_timer);
        } else {
            picker->motion_last_us = esp_timer_get_time();
            lv_timer_resume(picker->motion_timer);
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
    if (MediaService* media = MediaService::Existing(); media != nullptr) {
        media->UnloadOwner(state_);
    }
    if (RecordingService* recording = RecordingService::Existing();
        recording != nullptr) {
        recording->UnloadOwner(state_);
    }
    if (HttpService* http = HttpService::Existing(); http != nullptr) {
        http->CancelOwner(state_);
    }
    if (state_->theme_timer != nullptr) {
        lv_timer_delete(state_->theme_timer);
        state_->theme_timer = nullptr;
    }
    state_->theme_callback = nullptr;
    state_->theme_app_context = nullptr;
    for (const auto& interval : state_->intervals) {
        if (interval != nullptr && interval->timer != nullptr) {
            lv_timer_delete(interval->timer);
            interval->timer = nullptr;
        }
    }
    state_->intervals.clear();
    state_->pets.clear();
    if (state_->swipe_event_attached && state_->content != nullptr &&
        lv_obj_is_valid(state_->content)) {
        lv_obj_remove_event_cb_with_user_data(state_->content, OnContentSwipe,
                                              state_);
        state_->swipe_event_attached = false;
    }
    for (const auto& button : state_->buttons) {
        if (button != nullptr && button->button != nullptr &&
            lv_obj_is_valid(button->button)) {
            lv_obj_remove_event_cb_with_user_data(
                button->button, OnExternalButtonClicked, button.get());
        }
    }
    state_->buttons.clear();
    for (const auto& slider : state_->sliders) {
        if (slider != nullptr && slider->object != nullptr &&
            lv_obj_is_valid(slider->object)) {
            lv_obj_remove_event_cb_with_user_data(
                slider->object, GuardExternalSliderRelease, slider.get());
            lv_obj_remove_event_cb_with_user_data(
                slider->object, OnExternalSliderChanged, slider.get());
        }
    }
    state_->sliders.clear();
    for (const auto& segment : state_->segments) {
        if (segment == nullptr) continue;
        for (ExternalSegmentItem& item : segment->items) {
            if (item.button != nullptr && lv_obj_is_valid(item.button)) {
                lv_obj_remove_event_cb_with_user_data(
                    item.button, OnExternalSegmentClicked, &item);
            }
        }
    }
    state_->segments.clear();
    for (const auto& picker : state_->pickers) {
        if (picker == nullptr) continue;
        StopPickerMotion(picker.get());
        if (picker->object != nullptr && lv_obj_is_valid(picker->object)) {
            lv_obj_remove_event_cb_with_user_data(picker->object,
                                                  OnPickerPressed,
                                                  picker.get());
            lv_obj_remove_event_cb_with_user_data(picker->object,
                                                  OnPickerPressing,
                                                  picker.get());
            lv_obj_remove_event_cb_with_user_data(picker->object,
                                                  OnPickerReleased,
                                                  picker.get());
        }
    }
    state_->pickers.clear();
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
