#include "files_view.h"
#include "i18n.h"

#include <font_awesome.h>

#include "components/haptic_feedback.h"
#include "components/ui_components.h"
#include "core/app_shell.h"
#include "core/fonts.h"
#include "core/theme.h"
#include "core/ui_utils.h"
#include "apps/files/files_module.h"
#include "apps/files/files_list_window.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <esp_log.h>
#include "misc/cache/instance/lv_image_cache.h"
#include "misc/cache/instance/lv_image_header_cache.h"

namespace agent_ui {

namespace {

constexpr const char* TAG_SD = "FilesView";
constexpr int kPanelSize = 720;
constexpr int kHeaderH = agent_ui::metrics::kStatusBarHeight;
constexpr int kPad = agent_ui::metrics::kPagePadding;
constexpr size_t kMaxPathLen = files::kMaxPathLength;

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
std::string s_rendered_directory;
size_t s_rendered_page_offset = 0;

// 全屏预览层（图片 / 文本）
lv_obj_t* s_preview_overlay = nullptr;
lv_obj_t* s_preview_img = nullptr;
lv_obj_t* s_preview_text_scroll = nullptr;
lv_obj_t* s_preview_text_lbl = nullptr;
lv_obj_t* s_preview_title = nullptr;
char s_preview_lv_path[kMaxPathLen + 4] = {};  // "S:" + posix path

struct FileRowView;
std::vector<FileRowView*> s_file_rows;
lv_obj_t* s_file_scroll_extent = nullptr;
bool s_file_row_render_queued = false;

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
void UpdateStatusUI(const files::ViewState& state);
void UpdatePathLabel(const files::ViewState& state);
void RenderVisibleFileRows(const files::ViewState& state);

// 子目录：返回上一级；根目录：退出到首页；预览打开时先关预览
void ClosePreview();

void OnNavigateBack() {
    if (IsPreviewOpen()) {
        ClosePreview();
        return;
    }
    FilesModule::Get().NavigateBack();
}

void OnSwipeBack() {
    OnNavigateBack();
}

void OnBackClicked(lv_event_t*) { OnNavigateBack(); }

void UpdatePathLabel(const files::ViewState& state) {
    if (s_path_lbl == nullptr) {
        return;
    }
    if (state.root.empty() || state.directory == state.root) {
        lv_label_set_text(s_path_lbl, "/");
        return;
    }
    // 相对挂载点显示，如 /photos/2024
    if (state.directory.compare(0, state.root.size(), state.root) == 0) {
        lv_label_set_text(s_path_lbl, state.directory.c_str() + state.root.size());
    } else {
        lv_label_set_text(s_path_lbl, state.directory.c_str());
    }
}

// ----- file deletion -----
struct FileRowView {
    lv_obj_t* root = nullptr;
    lv_obj_t* name = nullptr;
    lv_obj_t* size = nullptr;
    size_t bound_index = static_cast<size_t>(-1);
    uint32_t bound_epoch = 0;
    uint32_t border_color = 0;
    uint32_t text_color = 0;
    uint32_t muted_color = 0;
    uint32_t pressed_color = 0;
};

// Forward declarations
void RefreshUsbUi();
void ScheduleVisibleFileRowRender(lv_event_t* event);

void OnUsbVirtualDiskClicked(lv_event_t* /*e*/) {
    FilesModule::Get().ToggleUsb();
}

void OnUsbUiNotifyAsync(void* /*user_data*/) {
    // 页面已卸载则忽略过期回调，避免碰已释放的 LVGL 对象
    if (s_screen == nullptr) {
        return;
    }
    RefreshUsbUi();
    FilesModule::Get().HandleUsbUiNotification();
}

void RefreshUsbUi() {
    const auto vd = FilesModule::Get().usb_state();
    if (s_usb_btn_lbl != nullptr) {
        const char* btn_text =
            vd.active ? I18n::T("停用虚拟 U 盘") : I18n::T("启用虚拟 U 盘");
        lv_label_set_text(s_usb_btn_lbl, btn_text);
    }
    if (s_usb_btn != nullptr) {
        const uint32_t bg = vd.active ? Colors().danger : Colors().accent;
        lv_obj_set_style_bg_color(s_usb_btn, lv_color_hex(bg), LV_PART_MAIN);
        if (vd.busy || !vd.supported) {
            lv_obj_add_state(s_usb_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_usb_btn, LV_STATE_DISABLED);
        }
    }
    const lv_color_t foreground =
        vd.active ? lv_color_white() : lv_color_hex(Colors().accent_ink);
    if (s_usb_btn_icon != nullptr) {
        lv_obj_set_style_text_color(s_usb_btn_icon, foreground, LV_PART_MAIN);
    }
    if (s_usb_btn_lbl != nullptr) {
        lv_obj_set_style_text_color(s_usb_btn_lbl, foreground, LV_PART_MAIN);
    }
}

void OnDeleteFile(lv_event_t* e) {
    (void)e;
    if (!FilesModule::Get().RequestDeletePreview(nullptr)) return;
    if (s_status_lbl != nullptr) lv_label_set_text(s_status_lbl, I18n::T("删除中…"));
}

void ClosePreviewVisualImpl() {
    if (s_preview_overlay != nullptr) {
        lv_obj_add_flag(s_preview_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_preview_img != nullptr) {
        lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
        // 清掉 src，避免继续持有大图解码缓存
        lv_image_set_src(s_preview_img, nullptr);
        if (s_preview_lv_path[0] != '\0') {
            lv_image_cache_drop(s_preview_lv_path);
            lv_image_header_cache_drop(s_preview_lv_path);
        }
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
}

void ClosePreview() {
    FilesModule::Get().ClosePreview();
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

bool OpenImagePreviewVisual(const char* posix_path) {
    if (s_preview_img == nullptr || posix_path == nullptr || posix_path[0] == '\0') {
        return false;
    }
    char owned_path[kMaxPathLen] = {};
    if (strlcpy(owned_path, posix_path, sizeof(owned_path)) >= sizeof(owned_path)) return false;
    // LVGL POSIX 驱动字母 S:，路径形如 S:/sdcard/foo.jpg（缓冲需在预览期间保持有效）
    const size_t path_len = strlen(owned_path);
    if (path_len + 3 > sizeof(s_preview_lv_path)) {
        ESP_LOGW(TAG_SD, "Image path too long: %s", posix_path);
        return false;
    }
    s_preview_lv_path[0] = 'S';
    s_preview_lv_path[1] = ':';
    memcpy(s_preview_lv_path + 2, owned_path, path_len + 1);
    const char* title = strrchr(owned_path, '/');
    SetPreviewTitle(title != nullptr ? title + 1 : owned_path);
    ESP_LOGI(TAG_SD, "Image preview: %s", s_preview_lv_path);

    if (s_preview_text_scroll != nullptr) {
        lv_obj_add_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
    }
    lv_image_set_src(s_preview_img, s_preview_lv_path);
    lv_obj_remove_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
    ShowPreviewOverlay();
    return true;
}

void OnFileRowClicked(lv_event_t* event) {
    auto* row = static_cast<FileRowView*>(lv_event_get_user_data(event));
    if (row == nullptr) return;
    if (!files_list_window::IsBindingCurrent(
            row->bound_epoch, FilesModule::Get().state().row_epoch)) {
        ScheduleVisibleFileRowRender(event);
        return;
    }
    FilesModule::Get().ActivateEntry(row->bound_index, row->bound_epoch);
}

void QueueVisibleFileRowRender(void*);

void OnFileListScroll(lv_event_t*) {
    QueueVisibleFileRowRender(nullptr);
}

// ----- file list builder -----
// Cast helper to silence -Wdeprecated-enum-enum-conversion
static inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

constexpr size_t kFileRowPoolCapacity = 16;
constexpr int kFileRowHeight = 76;

void RenderVisibleFileRows(const files::ViewState& state);
void EnsureFileScrollExtent(const files::ViewState& state) {
    if (s_file_list == nullptr) return;
    if (s_file_scroll_extent == nullptr) {
        s_file_scroll_extent = lv_obj_create(s_file_list);
        lv_obj_remove_style_all(s_file_scroll_extent);
        lv_obj_set_width(s_file_scroll_extent, kPanelSize - 2 * kPad);
        lv_obj_set_height(s_file_scroll_extent,
                          metrics::kBottomActionBarY - kListY);
        lv_obj_remove_flag(s_file_scroll_extent,
                           static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
                                                     LV_OBJ_FLAG_SCROLLABLE));
    }
    const size_t content_height = state.entries.size() * kFileRowHeight;
    const size_t minimum_height = static_cast<size_t>(
        std::max(0, metrics::kBottomActionBarY - kListY));
    const size_t height = std::max(minimum_height, content_height);
    lv_obj_set_height(s_file_scroll_extent,
                      static_cast<int>(std::min<size_t>(height, INT_MAX)));
}

void QueueVisibleFileRowRender(void*) {
    s_file_row_render_queued = false;
    RenderVisibleFileRows(FilesModule::Get().state());
}

void ScheduleVisibleFileRowRender(lv_event_t*) {
    if (s_file_row_render_queued) return;
    if (lv_async_call(QueueVisibleFileRowRender, nullptr) == LV_RESULT_OK) {
        s_file_row_render_queued = true;
    }
}

void BindFileRow(FileRowView* view, const files::DirectoryItem& item,
                 size_t index, const files::ViewState& state) {
    if (view == nullptr || view->root == nullptr) return;
    const bool changed = view->bound_index != index ||
                         view->bound_epoch != state.row_epoch;
    if (changed) {
        const auto& record = item.entry;
        const char* display_name = record.name.c_str();
        if (item.kind == files::ItemKind::PreviousPage)
            display_name = I18n::T("上一页");
        else if (item.kind == files::ItemKind::NextPage)
            display_name = I18n::T("下一页（还有更多文件）");
        else if (item.kind == files::ItemKind::Notice)
            display_name = I18n::T("部分名称或路径过长，无法打开");
        view->bound_index = index;
        view->bound_epoch = state.row_epoch;
        ui_components::SetLabelTextIfChanged(view->name, display_name);
        char size_str[64];
        if (item.kind != files::ItemKind::File) {
            size_str[0] = '\0';
        } else if (!record.path_usable) {
            snprintf(size_str, sizeof(size_str), I18n::T("无法打开此名称或路径"));
        } else if (record.is_directory) {
            snprintf(size_str, sizeof(size_str), I18n::T("目录"));
        } else {
            FormatSize(record.size, size_str, sizeof(size_str));
        }
        ui_components::SetLabelTextIfChanged(view->size, size_str);
    }
    const auto& colors = Colors();
    if (view->border_color != colors.border) {
        lv_obj_set_style_border_color(view->root, lv_color_hex(colors.border), LV_PART_MAIN);
        view->border_color = colors.border;
    }
    if (view->text_color != colors.text) {
        lv_obj_set_style_text_color(view->name, lv_color_hex(colors.text), LV_PART_MAIN);
        view->text_color = colors.text;
    }
    if (view->muted_color != colors.muted) {
        lv_obj_set_style_text_color(view->size, lv_color_hex(colors.muted), LV_PART_MAIN);
        view->muted_color = colors.muted;
    }
    if (view->pressed_color != colors.accent_pressed) {
        lv_obj_set_style_bg_color(view->root, lv_color_hex(colors.accent_pressed),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        view->pressed_color = colors.accent_pressed;
    }
    if (item.kind == files::ItemKind::PreviousPage ||
        item.kind == files::ItemKind::NextPage ||
        (item.kind == files::ItemKind::File && item.entry.path_usable &&
         (item.entry.is_directory || IsPreviewableFile(item.entry.name.c_str())))) {
        lv_obj_add_flag(view->root, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(view->root, LV_OBJ_FLAG_CLICKABLE);
    }
}

FileRowView* CreateFileRowSlot() {
    auto* view = new FileRowView;
    view->root = lv_obj_create(s_file_list);
    lv_obj_remove_style_all(view->root);
    lv_obj_set_size(view->root, kPanelSize - 2 * kPad, kFileRowHeight);
    lv_obj_set_style_bg_opa(view->root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(view->root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(view->root, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(view->root, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(view->root, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(view->root, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(view->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(view->root, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(view->root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* info_col = lv_obj_create(view->root);
    lv_obj_remove_style_all(info_col);
    lv_obj_set_size(info_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info_col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(info_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(info_col, 1);
    lv_obj_remove_flag(info_col, LV_OBJ_FLAG_CLICKABLE);

    view->name = lv_label_create(info_col);
    lv_label_set_long_mode(view->name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(view->name, 450);
    lv_obj_set_style_text_font(view->name, fonts::Medium(), LV_PART_MAIN);
    view->size = lv_label_create(info_col);
    lv_obj_set_style_text_font(view->size, fonts::Small(), LV_PART_MAIN);
    AttachButtonHaptic(view->root);
    lv_obj_add_event_cb(view->root, OnFileRowClicked, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(view->root, ScheduleVisibleFileRowRender,
                        LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(view->root, ScheduleVisibleFileRowRender,
                        LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(view->root, [](lv_event_t* event) {
        delete static_cast<FileRowView*>(lv_event_get_user_data(event));
    }, LV_EVENT_DELETE, view);
    lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
    s_file_rows.push_back(view);
    return view;
}

void RenderVisibleFileRows(const files::ViewState& state) {
    EnsureFileScrollExtent(state);
    if (s_file_list == nullptr || state.entries.empty()) {
        for (FileRowView* row : s_file_rows) {
            if (row != nullptr && row->root != nullptr)
                lv_obj_add_flag(row->root, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    using files_list_window::kNoItem;
    const auto range = files_list_window::CalculateRange(
        state.entries.size(), lv_obj_get_scroll_y(s_file_list),
        lv_obj_get_height(s_file_list), kFileRowHeight,
        kFileRowPoolCapacity);
    const auto assign_slots = [&]() {
        std::array<files_list_window::Slot, kFileRowPoolCapacity> slots{};
        slots.fill({kNoItem, false, false});
        for (size_t i = 0; i < s_file_rows.size(); ++i) {
            FileRowView* row = s_file_rows[i];
            slots[i] = {row != nullptr ? row->bound_index : kNoItem,
                        row != nullptr && row->root != nullptr &&
                            lv_obj_has_state(row->root, LV_STATE_PRESSED),
                        row != nullptr && row->root != nullptr};
        }
        return files_list_window::Assign(range, slots);
    };
    auto assignments = assign_slots();
    const auto range_covered = [&](const auto& current) {
        for (size_t item = range.first; item < range.first + range.count; ++item) {
            bool found = false;
            for (size_t bound : current) found = found || bound == item;
            if (!found) return false;
        }
        return true;
    };
    while (!range_covered(assignments) &&
           s_file_rows.size() < kFileRowPoolCapacity) {
        CreateFileRowSlot();
        assignments = assign_slots();
    }
    for (size_t slot = 0; slot < s_file_rows.size(); ++slot) {
        FileRowView* row = s_file_rows[slot];
        if (row == nullptr || row->root == nullptr) continue;
        const bool pressed = lv_obj_has_state(row->root, LV_STATE_PRESSED);
        const size_t item = assignments[slot];
        if (item == kNoItem) {
            if (!pressed) {
                lv_obj_add_flag(row->root, LV_OBJ_FLAG_HIDDEN);
                row->bound_index = kNoItem;
            }
            continue;
        }
        // Keep the object beneath an active pointer bound to its original
        // entry until LVGL releases or cancels the press. The queued render
        // then rebinds it after click dispatch completes.
        if (pressed) continue;
        BindFileRow(row, state.entries[item], item, state);
        lv_obj_set_pos(row->root, 0, static_cast<int>(item * kFileRowHeight));
        lv_obj_remove_flag(row->root, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_style_pad_all(s_file_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_file_list, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_file_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_file_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_file_list, OnFileListScroll, LV_EVENT_SCROLL, nullptr);
    EnsureFileScrollExtent(FilesModule::Get().state());
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
        [](lv_event_t*) {
            ClosePreview();
            FilesModule::Get().controller().RefreshStatus();
        });
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

void UpdateStatusUI(const files::ViewState& state) {
    const bool ready = state.status == files::Status::Mounted ||
                       state.status == files::Status::ReadError;
    if (s_status_dot != nullptr) {
        lv_obj_set_style_bg_color(s_status_dot,
                                  lv_color_hex(ready ? 0x00CC00 : 0xFF0000),
                                  LV_PART_MAIN);
    }
    if (s_status_lbl != nullptr) {
        const char* text = I18n::T("SD 卡已挂载");
        if (state.status == files::Status::Missing)
            text = I18n::T("未检测到 SD 卡");
        else if (state.status == files::Status::UsbBusy)
            text = I18n::T("SD 卡正在切换…");
        else if (state.status == files::Status::UsbExported)
            text = I18n::T("SD 正被电脑占用");
        lv_label_set_text(s_status_lbl, text);
    }
    if (s_capacity_lbl != nullptr) {
        if (state.status == files::Status::Missing) {
            lv_label_set_text(s_capacity_lbl, I18n::T("请插入 SD 卡"));
        } else if (state.status == files::Status::UsbBusy ||
                   state.status == files::Status::UsbExported) {
            lv_label_set_text(s_capacity_lbl, "");
        } else if (state.capacity_loading) {
            lv_label_set_text(s_capacity_lbl, I18n::T("正在读取容量…"));
        } else if (state.capacity_available) {
            char total_str[32], free_str[32], capacity[96];
            FormatSize(state.capacity_total, total_str, sizeof(total_str));
            FormatSize(state.capacity_free, free_str, sizeof(free_str));
            snprintf(capacity, sizeof(capacity), I18n::T("%s 可用 / %s"),
                     free_str, total_str);
            lv_label_set_text(s_capacity_lbl, capacity);
        } else {
            lv_label_set_text(s_capacity_lbl, "");
        }
    }
}

}  // namespace

lv_obj_t* FilesView::Create() { return FilesModule::Get().Create(); }

void FilesView::LifecycleCallback(AppLifecycleEvent event) {
    FilesModule::Get().LifecycleCallback(event);
}

lv_obj_t* FilesView::CreateWidgets() {
    auto shell = agent_ui::CreateAppShell("文件", "本地与 SD 卡", true, OnBackClicked);
    lv_obj_t* scr = shell.root;
    s_screen = scr;
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
    RefreshUsbUi();
    AttachSwipeBack(scr, OnSwipeBack);
    AttachAppLifecycle(scr, FilesView::LifecycleCallback);
    return scr;
}

void FilesView::OnUnloadWidgets() {
    if (s_file_row_render_queued) {
        lv_async_call_cancel(QueueVisibleFileRowRender, nullptr);
        s_file_row_render_queued = false;
    }
    s_screen = nullptr;
    s_usb_btn = nullptr;
    s_usb_btn_icon = nullptr;
    s_usb_btn_lbl = nullptr;
    s_status_lbl = nullptr;
    s_capacity_lbl = nullptr;
    s_path_lbl = nullptr;
    s_file_list = nullptr;
    s_file_scroll_extent = nullptr;
    s_file_rows.clear();
    s_rendered_directory.clear();
    s_no_files_lbl = nullptr;
    s_status_dot = nullptr;
    s_preview_overlay = nullptr;
    s_preview_img = nullptr;
    s_preview_text_scroll = nullptr;
    s_preview_text_lbl = nullptr;
    s_preview_title = nullptr;
    s_preview_lv_path[0] = '\0';
}

void FilesView::RenderState(const files::ViewState& state) {
    if (s_screen == nullptr) return;
    const bool same_page = s_rendered_directory == state.directory &&
                           s_rendered_page_offset == state.page_offset;
    const int previous_scroll = s_file_list != nullptr
                                    ? lv_obj_get_scroll_y(s_file_list) : 0;
    UpdateStatusUI(state);
    UpdatePathLabel(state);
    RefreshUsbUi();
    EnsureFileScrollExtent(state);
    RenderVisibleFileRows(state);
    if (s_file_list != nullptr) {
        lv_obj_scroll_to_y(s_file_list, same_page ? previous_scroll : 0, LV_ANIM_OFF);
    }
    s_rendered_directory = state.directory;
    s_rendered_page_offset = state.page_offset;

    if (s_no_files_lbl != nullptr) {
        const char* message = nullptr;
        if (state.status == files::Status::Missing)
            message = I18n::T("请插入 SD 卡");
        else if (state.status == files::Status::UsbExported)
            message = I18n::T("SD 正被电脑占用，停用或弹出后可浏览");
        else if (state.status == files::Status::UsbBusy)
            message = I18n::T("SD 卡正在切换…");
        else if (state.directory_loading)
            message = I18n::T("读取中…");
        else if (state.status == files::Status::ReadError)
            message = I18n::T("无法读取文件夹");
        else if (state.real_entry_count == 0 && !state.page_truncated)
            message = state.directory == state.root ? I18n::T("SD 卡内没有文件")
                                                    : I18n::T("此文件夹为空");
        if (message != nullptr) {
            lv_label_set_text(s_no_files_lbl, message);
            lv_obj_remove_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_no_files_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (state.preview_kind == files::PreviewKind::None) {
        ClosePreviewVisualImpl();
    } else if (state.preview_kind == files::PreviewKind::TextLoading ||
               state.preview_kind == files::PreviewKind::Text) {
        const std::size_t separator = state.preview_path.find_last_of('/');
        const char* base = separator == std::string::npos
                                ? state.preview_path.c_str()
                                : state.preview_path.c_str() + separator + 1;
        SetPreviewTitle(base != nullptr ? base : state.preview_path.c_str());
        if (s_preview_img != nullptr) {
            lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(s_preview_img, nullptr);
        }
        if (s_preview_text_lbl != nullptr && s_preview_text_scroll != nullptr) {
            if (state.preview_kind == files::PreviewKind::TextLoading) {
                lv_label_set_text(s_preview_text_lbl, I18n::T("读取中…"));
            } else if (state.preview_failed) {
                lv_label_set_text(s_preview_text_lbl, I18n::T("无法打开文件"));
            } else if (state.preview_truncated) {
                lv_label_set_text_fmt(s_preview_text_lbl, "%s\n\n%s",
                                      state.preview_text.c_str(),
                                      I18n::T("文件过大，已截断显示"));
            } else {
                lv_label_set_text(s_preview_text_lbl, state.preview_text.c_str());
            }
            lv_obj_scroll_to_y(s_preview_text_scroll, 0, LV_ANIM_OFF);
            lv_obj_remove_flag(s_preview_text_scroll, LV_OBJ_FLAG_HIDDEN);
            ShowPreviewOverlay();
        }
    }
}

bool FilesView::ShowImagePreview(const char* posix_path) {
    return OpenImagePreviewVisual(posix_path);
}

void FilesView::ClosePreviewVisual() { ClosePreviewVisualImpl(); }

void FilesView::ScheduleUsbStateRefresh() {
    if (s_screen != nullptr) lv_async_call(OnUsbUiNotifyAsync, nullptr);
}

bool FilesView::PreviewPath(const char* posix_path) {
    return FilesModule::Get().PreviewPath(posix_path);
}

bool FilesView::PreviewPath(
    const char* posix_path, const files_io_worker::TicketPtr& ticket) {
    return FilesModule::Get().PreviewPath(posix_path, ticket);
}

bool FilesView::IsPreviewFor(const char* posix_path) {
    return FilesModule::Get().IsPreviewFor(posix_path);
}

void FilesView::ApplyPendingIoResults() {
    FilesModule::Get().ApplyPendingIoResults();
}

bool FilesView::RequestDeletePath(
    const char* posix_path, const files_io_worker::TicketPtr& ticket) {
    return FilesModule::Get().RequestDeletePath(posix_path, ticket);
}

bool FilesView::DeletePath(const char* posix_path) {
    return FilesModule::Get().DeletePath(posix_path);
}

bool FilesView::GetStorageBytes(uint64_t* total_bytes, uint64_t* free_bytes) {
    return FilesModule::Get().GetStorageBytes(total_bytes, free_bytes);
}
}  // namespace agent_ui
