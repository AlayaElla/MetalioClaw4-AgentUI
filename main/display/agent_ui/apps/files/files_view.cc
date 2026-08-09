#include "files_view.h"
#include "i18n.h"

#include <font_awesome.h>

#include "components/haptic_feedback.h"
#include "components/ui_components.h"
#include "core/app_shell.h"
#include "core/fonts.h"
#include "core/navigation.h"
#include "core/theme.h"
#include "core/ui_utils.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <unistd.h>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <sdmmc_cmd.h>
#include "ff.h"

#include "SdCardManager.hpp"
#include "usb_virtual_disk.h"

namespace agent_ui {

namespace {

constexpr const char* TAG_SD = "FilesView";
constexpr int kPanelSize = 720;
constexpr int kHeaderH = agent_ui::metrics::kStatusBarHeight;
constexpr int kPad = agent_ui::metrics::kPagePadding;
constexpr size_t kMaxNameLen = 256;  // POSIX NAME_MAX = 255
constexpr size_t kMaxPathLen = 512;
constexpr size_t kMaxTextPreviewBytes = 48 * 1024;

const agent_ui::ThemeColors& Colors() { return agent_ui::Theme::Get().colors(); }

// ----- UI elements that need updating -----
lv_obj_t* s_status_lbl = nullptr;
lv_obj_t* s_capacity_lbl = nullptr;
lv_obj_t* s_path_lbl = nullptr;
lv_obj_t* s_file_list = nullptr;
lv_obj_t* s_no_files_lbl = nullptr;
lv_obj_t* s_status_dot = nullptr;
lv_obj_t* s_usb_btn = nullptr;
lv_obj_t* s_usb_btn_icon = nullptr;
lv_obj_t* s_usb_btn_lbl = nullptr;
lv_obj_t* s_screen = nullptr;

// 全屏预览层（图片 / 文本）
lv_obj_t* s_preview_overlay = nullptr;
lv_obj_t* s_preview_img = nullptr;
lv_obj_t* s_preview_text_scroll = nullptr;
lv_obj_t* s_preview_text_lbl = nullptr;
lv_obj_t* s_preview_title = nullptr;
char s_preview_lv_path[kMaxPathLen + 4] = {};  // "S:" + posix path
char s_preview_posix_path[kMaxPathLen] = {};

// 当前浏览目录（绝对路径，含挂载点前缀，如 /sdcard 或 /sdcard/photos）
char s_cwd[kMaxPathLen] = {};

constexpr int kDividerY = kHeaderH + 69;
constexpr int kPathY = kHeaderH + 86;
constexpr int kListY = kHeaderH + 122;

// ----- helper: human-readable size (integer-only, safe with newlib-nano) -----
void FormatSize(uint64_t bytes, char* buf, size_t buf_size) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        // e.g. "3.21 GB"
        unsigned gb_int = static_cast<unsigned>(bytes / (1024ULL * 1024 * 1024));
        unsigned gb_frac =
            static_cast<unsigned>((bytes % (1024ULL * 1024 * 1024)) / (10ULL * 1024 * 1024));
        snprintf(buf, buf_size, "%u.%02u GB", gb_int, gb_frac);
    } else if (bytes >= 1024 * 1024) {
        unsigned mb_int = static_cast<unsigned>(bytes / (1024 * 1024));
        unsigned mb_frac =
            static_cast<unsigned>((bytes % (1024 * 1024)) / (10 * 1024));
        snprintf(buf, buf_size, "%u.%02u MB", mb_int, mb_frac);
    } else if (bytes >= 1024) {
        unsigned kb_int = static_cast<unsigned>(bytes / 1024);
        unsigned kb_frac =
            static_cast<unsigned>(((bytes % 1024) * 100) / 1024);
        snprintf(buf, buf_size, "%u.%02u KB", kb_int, kb_frac);
    } else {
        // newlib-nano 不支持 PRIu64（会显示成 "lu B"），小尺寸用 unsigned long 即可
        snprintf(buf, buf_size, "%lu B", static_cast<unsigned long>(bytes));
    }
}

bool ExtEqualsIgnoreCase(const char* name, const char* ext) {
    const char* dot = strrchr(name, '.');
    if (dot == nullptr || ext == nullptr) {
        return false;
    }
    ++dot;
    while (*dot != '\0' && *ext != '\0') {
        if (std::tolower(static_cast<unsigned char>(*dot)) !=
            std::tolower(static_cast<unsigned char>(*ext))) {
            return false;
        }
        ++dot;
        ++ext;
    }
    return *dot == '\0' && *ext == '\0';
}

bool IsImageFile(const char* name) {
    return ExtEqualsIgnoreCase(name, "jpg") || ExtEqualsIgnoreCase(name, "jpeg") ||
           ExtEqualsIgnoreCase(name, "png") || ExtEqualsIgnoreCase(name, "sjpg");
}

bool IsTextFile(const char* name) {
    return ExtEqualsIgnoreCase(name, "txt");
}

bool IsPreviewableFile(const char* name) {
    return IsImageFile(name) || IsTextFile(name);
}

bool IsPreviewOpen() {
    return s_preview_overlay != nullptr &&
           !lv_obj_has_flag(s_preview_overlay, LV_OBJ_FLAG_HIDDEN);
}

// ----- navigation -----
void UpdatePathLabel();
void RebuildFileList(lv_obj_t* parent);

void ResetCwdToRoot() {
    const char* mount = SdCardManager::GetInstance().GetMountPoint();
    strlcpy(s_cwd, mount != nullptr ? mount : "/sdcard", sizeof(s_cwd));
}

bool IsAtRoot() {
    const char* mount = SdCardManager::GetInstance().GetMountPoint();
    if (mount == nullptr) {
        return true;
    }
    return strcmp(s_cwd, mount) == 0;
}

bool GoUpOneLevel() {
    if (IsAtRoot()) {
        return false;
    }
    const char* mount = SdCardManager::GetInstance().GetMountPoint();
    char* slash = strrchr(s_cwd, '/');
    if (slash == nullptr || mount == nullptr) {
        ResetCwdToRoot();
        return true;
    }
    // 截断到上一级；不得短于挂载点
    const size_t mount_len = strlen(mount);
    if (static_cast<size_t>(slash - s_cwd) <= mount_len) {
        strlcpy(s_cwd, mount, sizeof(s_cwd));
    } else {
        *slash = '\0';
    }
    return true;
}

void LeaveToHome() {
    UsbVirtualDisk::GetInstance().DisableIfActive();
    agent_ui::Navigation::Get().Back();
}

// 子目录：返回上一级；根目录：退出到首页；预览打开时先关预览
void ClosePreview();

void OnNavigateBack() {
    if (IsPreviewOpen()) {
        ClosePreview();
        return;
    }
    if (GoUpOneLevel()) {
        UpdatePathLabel();
        if (s_screen != nullptr) {
            RebuildFileList(s_screen);
        }
        return;
    }
    LeaveToHome();
}

void OnSwipeBack() {
    OnNavigateBack();
}

void OnBackClicked(lv_event_t*) { OnNavigateBack(); }

void UpdatePathLabel() {
    if (s_path_lbl == nullptr) {
        return;
    }
    const char* mount = SdCardManager::GetInstance().GetMountPoint();
    if (mount == nullptr || IsAtRoot()) {
        lv_label_set_text(s_path_lbl, "/");
        return;
    }
    // 相对挂载点显示，如 /photos/2024
    const size_t mount_len = strlen(mount);
    if (strncmp(s_cwd, mount, mount_len) == 0) {
        lv_label_set_text(s_path_lbl, s_cwd + mount_len);
    } else {
        lv_label_set_text(s_path_lbl, s_cwd);
    }
}

// ----- file deletion -----
struct FileEntry {
    char name[kMaxNameLen];
    char path[kMaxPathLen];
};

// Forward declarations
void UpdateStatusUI();
void RefreshUsbUi();

void OnUsbVirtualDiskClicked(lv_event_t* /*e*/) {
    auto& vd = UsbVirtualDisk::GetInstance();
    if (!vd.IsSupported() || vd.IsBusy()) {
        return;
    }
    vd.Toggle();
}

void OnUsbUiNotifyAsync(void* /*user_data*/) {
    // 页面已卸载则忽略过期回调，避免碰已释放的 LVGL 对象
    if (s_screen == nullptr) {
        return;
    }
    ClosePreview();
    RefreshUsbUi();
    UpdateStatusUI();
    RebuildFileList(s_screen);
}

void OnUsbVirtualDiskNotify() {
    lv_async_call(OnUsbUiNotifyAsync, nullptr);
}

void RefreshUsbUi() {
    auto& vd = UsbVirtualDisk::GetInstance();
    if (s_usb_btn_lbl != nullptr) {
        const char* btn_text =
            vd.IsGadgetActive() ? I18n::T("停用虚拟 U 盘") : I18n::T("启用虚拟 U 盘");
        lv_label_set_text(s_usb_btn_lbl, btn_text);
    }
    if (s_usb_btn != nullptr) {
        const uint32_t bg = vd.IsGadgetActive() ? Colors().danger : Colors().accent;
        lv_obj_set_style_bg_color(s_usb_btn, lv_color_hex(bg), LV_PART_MAIN);
        if (vd.IsBusy() || !vd.IsSupported()) {
            lv_obj_add_state(s_usb_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_usb_btn, LV_STATE_DISABLED);
        }
    }
    const lv_color_t foreground =
        vd.IsGadgetActive() ? lv_color_white() : lv_color_hex(Colors().accent_ink);
    if (s_usb_btn_icon != nullptr) {
        lv_obj_set_style_text_color(s_usb_btn_icon, foreground, LV_PART_MAIN);
    }
    if (s_usb_btn_lbl != nullptr) {
        lv_obj_set_style_text_color(s_usb_btn_lbl, foreground, LV_PART_MAIN);
    }
}

void OnDeleteFile(lv_event_t* e) {
    auto& vd = UsbVirtualDisk::GetInstance();
    // 仅在 SD 被主机占用或切换中禁止删除
    if (vd.IsSdExportedToHost() || vd.IsBusy()) {
        return;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    auto* entry = static_cast<FileEntry*>(lv_event_get_user_data(e));
    const char* path = entry != nullptr ? entry->path : s_preview_posix_path;
    if (path == nullptr || path[0] == '\0') return;

    ESP_LOGI(TAG_SD, "Deleting file: %s", path);
    if (unlink(path) != 0) {
        ESP_LOGE(TAG_SD, "Failed to delete: %s", path);
        return;
    }

    ClosePreview();
    UpdateStatusUI();
    if (s_screen != nullptr) {
        RebuildFileList(s_screen);
    }
}

void ClosePreview() {
    if (s_preview_overlay != nullptr) {
        lv_obj_add_flag(s_preview_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_preview_img != nullptr) {
        lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
        // 清掉 src，避免继续持有大图解码缓存
        lv_image_set_src(s_preview_img, nullptr);
    }
    if (s_preview_text_scroll != nullptr) {
        lv_obj_add_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_preview_text_lbl != nullptr) {
        lv_label_set_text(s_preview_text_lbl, "");
    }
    if (s_preview_title != nullptr) {
        lv_label_set_text(s_preview_title, "");
    }
    s_preview_lv_path[0] = '\0';
    s_preview_posix_path[0] = '\0';
}

void SetPreviewTitle(const char* name) {
    if (s_preview_title == nullptr) {
        return;
    }
    lv_label_set_text(s_preview_title, (name != nullptr && name[0] != '\0') ? name : "");
}

void ShowPreviewOverlay() {
    if (s_preview_overlay == nullptr) {
        return;
    }
    lv_obj_remove_flag(s_preview_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_preview_overlay);
}

void OpenImagePreview(const char* posix_path) {
    if (s_preview_img == nullptr || posix_path == nullptr || posix_path[0] == '\0') {
        return;
    }
    // LVGL POSIX 驱动字母 S:，路径形如 S:/sdcard/foo.jpg（缓冲需在预览期间保持有效）
    const size_t path_len = strlen(posix_path);
    if (path_len + 3 > sizeof(s_preview_lv_path)) {
        ESP_LOGW(TAG_SD, "Image path too long: %s", posix_path);
        return;
    }
    s_preview_lv_path[0] = 'S';
    s_preview_lv_path[1] = ':';
    memcpy(s_preview_lv_path + 2, posix_path, path_len + 1);
    ESP_LOGI(TAG_SD, "Image preview: %s", s_preview_lv_path);

    if (s_preview_text_scroll != nullptr) {
        lv_obj_add_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
    }
    lv_image_set_src(s_preview_img, s_preview_lv_path);
    lv_obj_remove_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
    ShowPreviewOverlay();
}

void OpenTextPreview(const char* posix_path) {
    if (s_preview_text_lbl == nullptr || s_preview_text_scroll == nullptr ||
        posix_path == nullptr) {
        return;
    }

    FILE* fp = fopen(posix_path, "rb");
    if (fp == nullptr) {
        ESP_LOGE(TAG_SD, "Failed to open text: %s", posix_path);
        if (s_preview_img != nullptr) {
            lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(s_preview_text_lbl, I18n::T("无法打开文件"));
        lv_obj_remove_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
        ShowPreviewOverlay();
        return;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        lv_label_set_text(s_preview_text_lbl, I18n::T("无法打开文件"));
        lv_obj_remove_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
        ShowPreviewOverlay();
        return;
    }
    long file_size = ftell(fp);
    if (file_size < 0) {
        fclose(fp);
        lv_label_set_text(s_preview_text_lbl, I18n::T("无法打开文件"));
        lv_obj_remove_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
        ShowPreviewOverlay();
        return;
    }
    rewind(fp);

    const bool truncated = static_cast<size_t>(file_size) > kMaxTextPreviewBytes;
    size_t read_len = truncated ? kMaxTextPreviewBytes : static_cast<size_t>(file_size);
    // +80 for optional truncation note
    char* buf = new char[read_len + 128];
    size_t n = fread(buf, 1, read_len, fp);
    fclose(fp);
    buf[n] = '\0';
    // 粗略去掉中间的 NUL，避免 label 提前截断
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') {
            buf[i] = ' ';
        }
    }
    if (truncated) {
        snprintf(buf + n, 128, "\n\n%s", I18n::T("文件过大，已截断显示"));
    }

    ESP_LOGI(TAG_SD, "Text preview: %s (%u bytes%s)", posix_path,
             static_cast<unsigned>(n), truncated ? ", truncated" : "");

    if (s_preview_img != nullptr) {
        lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_preview_img, nullptr);
    }
    lv_label_set_text(s_preview_text_lbl, buf);
    delete[] buf;
    lv_obj_scroll_to_y(s_preview_text_scroll, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
    ShowPreviewOverlay();
}

void OnPreviewFile(lv_event_t* e) {
    auto& vd = UsbVirtualDisk::GetInstance();
    if (vd.IsSdExportedToHost() || vd.IsBusy()) {
        return;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    auto* entry = static_cast<FileEntry*>(lv_event_get_user_data(e));
    if (entry == nullptr) {
        return;
    }
    strlcpy(s_preview_posix_path, entry->path, sizeof(s_preview_posix_path));
    if (IsImageFile(entry->name)) {
        SetPreviewTitle(entry->name);
        OpenImagePreview(entry->path);
    } else if (IsTextFile(entry->name)) {
        SetPreviewTitle(entry->name);
        OpenTextPreview(entry->path);
    }
}

void OnEnterDirectory(lv_event_t* e) {
    auto& vd = UsbVirtualDisk::GetInstance();
    if (vd.IsSdExportedToHost() || vd.IsBusy()) {
        return;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    auto* entry = static_cast<FileEntry*>(lv_event_get_user_data(e));
    if (entry == nullptr || entry->path[0] == '\0') {
        return;
    }

    struct stat st;
    if (stat(entry->path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG_SD, "Not a directory: %s", entry->path);
        return;
    }

    ESP_LOGI(TAG_SD, "Enter directory: %s", entry->path);
    strlcpy(s_cwd, entry->path, sizeof(s_cwd));
    UpdatePathLabel();
    if (s_screen != nullptr) {
        RebuildFileList(s_screen);
    }
}

// ----- file list builder -----
void ClearFileList() {
    if (s_file_list == nullptr)
        return;
    lv_obj_clean(s_file_list);
}

// Cast helper to silence -Wdeprecated-enum-enum-conversion
static inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

// Build a single file row.  Stat is done inside this function so the caller
// doesn't need a large stack buffer.
void BuildFileRow(const char* name, const char* path) {
    struct stat st;
    bool is_dir = false;
    uint64_t size = 0;
    if (stat(path, &st) == 0) {
        is_dir = S_ISDIR(st.st_mode);
        size = st.st_size;
    }

    // Row container
    lv_obj_t* row = lv_obj_create(s_file_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, kPanelSize - 2 * kPad, 76);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(Colors().border), LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // File info column (name + size)
    lv_obj_t* info_col = lv_obj_create(row);
    lv_obj_remove_style_all(info_col);
    lv_obj_set_size(info_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info_col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(info_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(info_col, 1);
    lv_obj_remove_flag(info_col, LV_OBJ_FLAG_CLICKABLE);

    // File/dir name
    lv_obj_t* name_lbl = lv_label_create(info_col);
    lv_label_set_text(name_lbl, name);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_lbl, 450);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(Colors().text), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_lbl, fonts::Medium(), LV_PART_MAIN);

    // File size
    char size_str[64];
    if (is_dir) {
        snprintf(size_str, sizeof(size_str), I18n::T("目录"));
    } else {
        FormatSize(size, size_str, sizeof(size_str));
    }
    lv_obj_t* size_lbl = lv_label_create(info_col);
    lv_label_set_text(size_lbl, size_str);
    lv_obj_set_style_text_color(size_lbl, lv_color_hex(Colors().muted), LV_PART_MAIN);
    lv_obj_set_style_text_font(size_lbl, fonts::Small(), LV_PART_MAIN);

    if (is_dir) {
        // 目录行可点进入
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(Colors().accent_pressed),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));

        auto* dir_ctx = new FileEntry;
        strlcpy(dir_ctx->name, name, sizeof(dir_ctx->name));
        strlcpy(dir_ctx->path, path, sizeof(dir_ctx->path));
        lv_obj_add_event_cb(row, OnEnterDirectory, LV_EVENT_CLICKED, dir_ctx);
        AttachButtonHaptic(row);
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) { delete static_cast<FileEntry*>(lv_event_get_user_data(ev)); },
            LV_EVENT_DELETE, dir_ctx);
    } else {
        // jpg/png/sjpg/txt：点行打开预览
        if (IsPreviewableFile(name)) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(row, lv_color_hex(Colors().accent_pressed),
                                      Sel(LV_PART_MAIN, LV_STATE_PRESSED));
            auto* preview_ctx = new FileEntry;
            strlcpy(preview_ctx->name, name, sizeof(preview_ctx->name));
            strlcpy(preview_ctx->path, path, sizeof(preview_ctx->path));
            lv_obj_add_event_cb(row, OnPreviewFile, LV_EVENT_CLICKED, preview_ctx);
            AttachButtonHaptic(row);
            lv_obj_add_event_cb(
                row,
                [](lv_event_t* ev) {
                    delete static_cast<FileEntry*>(lv_event_get_user_data(ev));
                },
                LV_EVENT_DELETE, preview_ctx);
        }
    }
}

// 拼 cwd/name；超长则返回 false（跳过该条目，避免路径截断）
bool JoinPath(char* out, size_t out_size, const char* dir, const char* name) {
    if (out == nullptr || out_size == 0 || dir == nullptr || name == nullptr) {
        return false;
    }
    const size_t dir_len = strlen(dir);
    const size_t name_len = strlen(name);
    // dir + '/' + name + '\0'
    if (dir_len + 1 + name_len + 1 > out_size) {
        return false;
    }
    memcpy(out, dir, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, name, name_len + 1);
    return true;
}

void AppendDirEntries(DIR* dir, bool want_dirs) {
    char name_buf[kMaxNameLen];
    char path_buf[kMaxPathLen];
    struct dirent* entry;

    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        strlcpy(name_buf, entry->d_name, sizeof(name_buf));
        if (!JoinPath(path_buf, sizeof(path_buf), s_cwd, name_buf)) {
            ESP_LOGW(TAG_SD, "Skip path too long under '%s': %s", s_cwd, name_buf);
            continue;
        }

        struct stat st;
        bool is_dir = false;
        if (stat(path_buf, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
        }
        if (is_dir != want_dirs) {
            continue;
        }
        BuildFileRow(name_buf, path_buf);
    }
}

void RebuildFileList(lv_obj_t* parent) {
    (void)parent;
    ClearFileList();

    auto& vd = UsbVirtualDisk::GetInstance();
    if (vd.IsSdExportedToHost()) {
        if (s_no_files_lbl != nullptr) {
            lv_label_set_text(s_no_files_lbl, I18n::T("SD 正被电脑占用，停用或弹出后可浏览"));
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (!SdCardManager::GetInstance().IsMounted()) {
        if (s_no_files_lbl != nullptr) {
            lv_label_set_text(s_no_files_lbl, I18n::T("请插入 SD 卡"));
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (s_cwd[0] == '\0') {
        ResetCwdToRoot();
    }

    DIR* dir = opendir(s_cwd);
    if (dir == nullptr) {
        ESP_LOGW(TAG_SD, "Failed to open cwd '%s', reset to root", s_cwd);
        ResetCwdToRoot();
        UpdatePathLabel();
        dir = opendir(s_cwd);
    }
    if (dir == nullptr) {
        ESP_LOGE(TAG_SD, "Failed to open directory: %s", s_cwd);
        if (s_no_files_lbl != nullptr) {
            lv_label_set_text(s_no_files_lbl,
                              IsAtRoot() ? I18n::T("SD 卡内没有文件")
                                         : I18n::T("此文件夹为空"));
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // 先列目录、再列文件，方便浏览
    AppendDirEntries(dir, true);
    rewinddir(dir);
    AppendDirEntries(dir, false);
    closedir(dir);

    const bool has_files =
        (s_file_list != nullptr) && (lv_obj_get_child_count(s_file_list) > 0);
    if (!has_files) {
        if (s_no_files_lbl != nullptr) {
            lv_label_set_text(s_no_files_lbl,
                              IsAtRoot() ? I18n::T("SD 卡内没有文件")
                                         : I18n::T("此文件夹为空"));
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (s_no_files_lbl != nullptr) {
            lv_obj_add_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ----- status section -----
void BuildStatusSection(lv_obj_t* parent) {
    // SD state and capacity stay on one compact line so the file list gets more room.
    lv_obj_t* status_row = lv_obj_create(parent);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, kPanelSize - 2 * kPad, 69);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 10, LV_PART_MAIN);
    lv_obj_set_pos(status_row, kPad, kHeaderH);

    // Status dot (colored circle)
    s_status_dot = lv_obj_create(status_row);
    lv_obj_remove_style_all(s_status_dot);
    lv_obj_set_size(s_status_dot, 12, 12);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0xFF0000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, LV_PART_MAIN);

    // Status text
    s_status_lbl = lv_label_create(status_row);
    lv_label_set_text(s_status_lbl, I18n::T("检测中..."));
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(Colors().text), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_lbl, fonts::MediumBold(), LV_PART_MAIN);

    // Capacity label
    s_capacity_lbl = lv_label_create(status_row);
    lv_label_set_text(s_capacity_lbl, "");
    lv_obj_set_style_text_color(s_capacity_lbl, lv_color_hex(Colors().muted), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_capacity_lbl, fonts::Small(), LV_PART_MAIN);

    // Divider line
    lv_obj_t* divider = lv_obj_create(parent);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, kPanelSize, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(Colors().border), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_pos(divider, 0, kDividerY);
}

// Build the file list container
void BuildFileListSection(lv_obj_t* parent) {
    // 当前路径（相对挂载点，如 / 或 /photos）
    s_path_lbl = lv_label_create(parent);
    lv_label_set_text(s_path_lbl, "/");
    lv_label_set_long_mode(s_path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_path_lbl, kPanelSize - 2 * kPad);
    lv_obj_set_style_text_color(s_path_lbl, lv_color_hex(Colors().text), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_path_lbl, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_pos(s_path_lbl, kPad, kPathY);

    // "No files" placeholder
    s_no_files_lbl = lv_label_create(parent);
    lv_label_set_text(s_no_files_lbl, "");
    lv_obj_set_style_text_color(s_no_files_lbl, lv_color_hex(Colors().border), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_no_files_lbl, fonts::Small(), LV_PART_MAIN);
    lv_obj_align(s_no_files_lbl, LV_ALIGN_CENTER, 0, 40);

    // Scrollable file list
    constexpr int kListH = metrics::kBottomActionBarY - kListY;
    s_file_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_file_list);
    lv_obj_set_size(s_file_list, kPanelSize - 2 * kPad, kListH);
    lv_obj_set_pos(s_file_list, kPad, kListY);
    lv_obj_set_style_bg_opa(s_file_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_file_list, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_file_list, LV_SCROLLBAR_MODE_OFF);
}

void BuildPreviewOverlay(lv_obj_t* parent) {
    s_preview_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_preview_overlay);
    lv_obj_add_flag(s_preview_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_preview_overlay, kPanelSize,
                    kPanelSize - metrics::kStatusBarHeight);
    lv_obj_set_pos(s_preview_overlay, 0, metrics::kStatusBarHeight);
    lv_obj_set_style_bg_color(s_preview_overlay, lv_color_hex(Colors().background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_preview_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_preview_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_preview_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_preview_overlay, LV_OBJ_FLAG_HIDDEN);
    IgnoreSwipeBack(s_preview_overlay, true);

    lv_obj_t* actions = ui_components::CreateBottomActionBar(
        s_preview_overlay, metrics::kBottomActionContentHeight);
    ui_components::AddBottomActionButton(
        actions, FONT_AWESOME_ARROW_LEFT, I18n::T("返回"),
        [](lv_event_t*) { ClosePreview(); });
    ui_components::AddBottomActionSpacer(actions);
    ui_components::AddBottomActionButton(
        actions, FONT_AWESOME_TRASH, I18n::T("删除"), OnDeleteFile,
        nullptr, true);

    // 图片预览（全屏居中）
    s_preview_img = lv_image_create(s_preview_overlay);
    lv_obj_set_size(s_preview_img, kPanelSize,
                    metrics::kBottomActionContentHeight);
    lv_obj_set_pos(s_preview_img, 0, 0);
    lv_image_set_inner_align(s_preview_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_remove_flag(s_preview_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);

    // 文本预览（可上下滚动）
    s_preview_text_scroll = lv_obj_create(s_preview_overlay);
    lv_obj_remove_style_all(s_preview_text_scroll);
    lv_obj_set_size(s_preview_text_scroll, kPanelSize - 2 * kPad,
                    metrics::kBottomActionContentHeight - 2 * kPad);
    lv_obj_set_pos(s_preview_text_scroll, kPad, kPad);
    lv_obj_set_style_bg_opa(s_preview_text_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_preview_text_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_preview_text_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_preview_text_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
    IgnoreSwipeBack(s_preview_text_scroll, true);

    s_preview_text_lbl = lv_label_create(s_preview_text_scroll);
    lv_obj_set_width(s_preview_text_lbl, kPanelSize - 2 * kPad);
    lv_label_set_long_mode(s_preview_text_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_preview_text_lbl, lv_color_hex(Colors().text),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_preview_text_lbl, fonts::Small(), LV_PART_MAIN);
    lv_label_set_text(s_preview_text_lbl, "");
}

void UpdateStatusUI() {
    auto& sd = SdCardManager::GetInstance();
    auto& vd = UsbVirtualDisk::GetInstance();
    sdmmc_card_t* card = sd.GetCard();
    const bool usable = sd.IsMounted() && card != nullptr && !vd.IsSdExportedToHost();
    if (usable || (card != nullptr && vd.IsGadgetActive())) {
        if (s_status_dot != nullptr) {
            lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0x00CC00), LV_PART_MAIN);
        }
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, I18n::T("SD 卡已挂载"));
        }

        // Update capacity using FatFs free-cluster info (only when APP-mounted)
        if (s_capacity_lbl != nullptr) {
            if (!usable) {
                lv_label_set_text(s_capacity_lbl, "");
            } else {
                FATFS* fs = nullptr;
                DWORD free_clusters = 0;
                uint64_t total_bytes = 0;
                uint64_t free_bytes = 0;

                FRESULT res = f_getfree("0:", &free_clusters, &fs);
                if (res == FR_OK && fs != nullptr) {
                    // FatFs csize is in sectors; convert to bytes
                    DWORD ssize = 512;  // default sector size for SD cards
                    total_bytes = (uint64_t)(fs->n_fatent - 2) * fs->csize * ssize;
                    free_bytes = (uint64_t)free_clusters * fs->csize * ssize;
                } else {
                    // Fallback: use CSD capacity
                    total_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
                    free_bytes = 0;
                }

                char total_str[32], free_str[32];
                FormatSize(total_bytes, total_str, sizeof(total_str));
                FormatSize(free_bytes, free_str, sizeof(free_str));

                char cap_buf[96];
                snprintf(cap_buf, sizeof(cap_buf), I18n::T("%s 可用 / %s"), free_str,
                         total_str);
                lv_label_set_text(s_capacity_lbl, cap_buf);
            }
        }
    } else {
        if (s_status_dot != nullptr) {
            lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0xFF0000), LV_PART_MAIN);
        }
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, I18n::T("未检测到 SD 卡"));
        }
        if (s_capacity_lbl != nullptr) {
            lv_label_set_text(s_capacity_lbl, I18n::T("请插入 SD 卡"));
        }
        if (s_no_files_lbl != nullptr) {
            lv_label_set_text(s_no_files_lbl, I18n::T("请插入 SD 卡"));
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

}  // namespace

lv_obj_t* FilesView::Create() {
    auto shell = agent_ui::CreateAppShell("文件", "本地与 SD 卡", true, OnBackClicked);
    lv_obj_t* scr = shell.root;
    s_screen = scr;

    ResetCwdToRoot();

    BuildStatusSection(scr);
    BuildFileListSection(scr);
    auto usb_action = ui_components::AddBottomPrimaryButton(
        shell.actions, FONT_AWESOME_SD_CARD, I18n::T("启用虚拟 U 盘"),
        OnUsbVirtualDiskClicked);
    s_usb_btn = usb_action.root;
    s_usb_btn_icon = usb_action.icon;
    s_usb_btn_lbl = usb_action.label;
    lv_obj_t* trailing_placeholder = lv_obj_create(shell.actions);
    lv_obj_remove_style_all(trailing_placeholder);
    lv_obj_set_size(trailing_placeholder, 72, metrics::kBottomActionHeight);
    lv_obj_remove_flag(trailing_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    BuildPreviewOverlay(scr);

    auto& vd = UsbVirtualDisk::GetInstance();
    vd.Init();
    vd.SetUiNotify(OnUsbVirtualDiskNotify);
    RefreshUsbUi();

    // SD 卡的挂载已经在板级 init（METALIO_CLAW_4::InitializeSdCard()）里完成。
    // 这里只做一次状态读取并刷新 UI；如果开机时 mount 失败（卡没插），就只
    // 显示状态文字、不去重试，等用户回到首页 / 后续手动 reboot 时再处理。
    UpdateStatusUI();
    UpdatePathLabel();
    RebuildFileList(scr);

    // Right-swipe：子目录上一级，根目录回首页
    AttachSwipeBack(scr, OnSwipeBack);
    AttachAppLifecycle(scr, FilesView::LifecycleCallback);

    return scr;
}

void FilesView::LifecycleCallback(AppLifecycleEvent event) {
    if (event == AppLifecycleEvent::Load ||
        event == AppLifecycleEvent::Resume) {
        ESP_LOGI(TAG_SD, "%s: files_app (mounted=%d gadget=%d cwd=%s)",
                 event == AppLifecycleEvent::Load ? "load" : "resume",
                 SdCardManager::GetInstance().IsMounted() ? 1 : 0,
                 UsbVirtualDisk::GetInstance().IsGadgetActive() ? 1 : 0, s_cwd);
        UsbVirtualDisk::GetInstance().SetUiNotify(OnUsbVirtualDiskNotify);
        RefreshUsbUi();
        UpdateStatusUI();
        UpdatePathLabel();
        if (event == AppLifecycleEvent::Load && s_screen != nullptr) {
            RebuildFileList(s_screen);
        }
    } else if (event == AppLifecycleEvent::Suspend) {
        // Keep the mounted LVGL tree and path/preview state intact. There is
        // no periodic file worker to stop; only detach USB UI callbacks while
        // the screen cannot be interacted with.
        ESP_LOGI(TAG_SD, "suspend: files_app");
        UsbVirtualDisk::GetInstance().SetUiNotify(nullptr);
    } else {
        ESP_LOGI(TAG_SD, "unload: files_app");
        UsbVirtualDisk::GetInstance().DisableIfActive();
        UsbVirtualDisk::GetInstance().SetUiNotify(nullptr);
        ClosePreview();
        s_screen = nullptr;
        s_usb_btn = nullptr;
        s_usb_btn_icon = nullptr;
        s_usb_btn_lbl = nullptr;
        s_status_lbl = nullptr;
        s_capacity_lbl = nullptr;
        s_path_lbl = nullptr;
        s_file_list = nullptr;
        s_no_files_lbl = nullptr;
        s_status_dot = nullptr;
        s_preview_overlay = nullptr;
        s_preview_img = nullptr;
        s_preview_text_scroll = nullptr;
        s_preview_text_lbl = nullptr;
        s_preview_title = nullptr;
        s_preview_lv_path[0] = '\0';
        s_preview_posix_path[0] = '\0';
        s_cwd[0] = '\0';
    }
}

}  // namespace agent_ui
