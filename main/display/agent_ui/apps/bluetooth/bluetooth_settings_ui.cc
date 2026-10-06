#include "bluetooth_settings_ui.h"

#include <cstdio>
#include <algorithm>
#include <font_awesome.h>

#include "components/ui_components.h"
#include "core/fonts.h"
#include "core/theme.h"
#include "core/ui_utils.h"
#include "i18n.h"

namespace agent_ui::bluetooth_settings_ui {
namespace {

namespace ui = ui_components;

}  // namespace

void View::Build(lv_obj_t* parent, const Model& model,
                 const Callbacks& callbacks) {
    Reset();
    ui::CreateSectionHeading(parent, I18n::T("蓝牙"));
    lv_obj_t* master = ui::CreateRow(
        parent, FONT_AWESOME_BLUETOOTH,
        model.bluetooth_enabled ? I18n::T("蓝牙模式 · 已开启")
                                : I18n::T("蓝牙模式 · 已关闭"),
        nullptr,
        104);
    master_status_ = lv_obj_get_child(master, 1);
    master_switch_ = ui::AddSwitch(master, model.bluetooth_enabled,
                                   callbacks.master_changed);

    local_panel_ = lv_obj_create(parent);
    lv_obj_remove_style_all(local_panel_);
    lv_obj_set_size(local_panel_, LV_PCT(100), 116);
    lv_obj_set_style_border_color(local_panel_,
                                  lv_color_hex(Theme::Get().colors().border),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(local_panel_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(
        local_panel_,
        static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM),
        LV_PART_MAIN);
    lv_obj_remove_flag(local_panel_, LV_OBJ_FLAG_SCROLLABLE);
    auto local = ui::CreateCompactRow(
        local_panel_, FONT_AWESOME_VOLUME_HIGH, I18n::T("本机扬声器"),
        nullptr, nullptr, 116, false, false);
    lv_obj_set_style_pad_hor(local.root, 20, LV_PART_MAIN);

    settings_panel_ = ui::CreateContentPanel(parent, LV_SIZE_CONTENT, 12);
    speaker_panel_ = ui::CreateContentPanel(settings_panel_, LV_SIZE_CONTENT);
    auto current_toolbar = ui::CreateToolbar(
        speaker_panel_, I18n::T("当前连接设备 · 0"), nullptr);
    current_count_ = current_toolbar.title;
    current_list_ = ui::CreateDividerList(speaker_panel_, 72);
    lv_obj_add_flag(current_list_, LV_OBJ_FLAG_HIDDEN);

    ui::CreateToolbar(speaker_panel_, I18n::T("音频模式"), nullptr);
    lv_obj_t* profile = ui::CreateSegment(speaker_panel_, 64);
    profile_buttons_[0] = ui::AddSegmentButton(
        profile, FONT_AWESOME_VOLUME_HIGH, I18n::T("音乐模式"),
        model.profile_selected && !model.call_profile,
        callbacks.music_profile);
    profile_buttons_[1] = ui::AddSegmentButton(
        profile, FONT_AWESOME_PHONE, I18n::T("通话模式"),
        model.profile_selected && model.call_profile,
        callbacks.call_profile);

    auto nearby_toolbar = ui::CreateToolbar(
        speaker_panel_, I18n::T("附近音响 · 0"), nullptr);
    nearby_count_ = nearby_toolbar.title;
    device_list_ = ui::CreateDividerList(speaker_panel_, 190);
    lv_obj_set_style_border_width(device_list_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(device_list_, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(device_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(device_list_, LV_SCROLLBAR_MODE_OFF);
    IgnoreSwipeBack(device_list_, true);
    nearby_spinner_ = lv_spinner_create(device_list_);
    lv_obj_set_size(nearby_spinner_, 64, 64);
    const auto& colors = Theme::Get().colors();
    lv_obj_set_style_arc_color(nearby_spinner_, lv_color_hex(colors.border),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(nearby_spinner_, lv_color_hex(colors.accent),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(nearby_spinner_, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(nearby_spinner_, 7, LV_PART_INDICATOR);
    lv_obj_add_flag(nearby_spinner_, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(nearby_spinner_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(nearby_spinner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(device_list_, LV_OBJ_FLAG_HIDDEN);
    auto scan = ui::AddWideActionButton(
        speaker_panel_, FONT_AWESOME_ARROWS_ROTATE,
        I18n::T("扫描音响"), callbacks.scan);
    scan_button_ = scan.root;
    scan_label_ = scan.label;

    SetBluetoothEnabled(model.bluetooth_enabled);
    SetAudioProfile(model.connected, model.profile_selected,
                    model.call_profile);
}

void View::Reset() {
    master_status_ = nullptr;
    master_switch_ = nullptr;
    local_panel_ = nullptr;
    settings_panel_ = nullptr;
    speaker_panel_ = nullptr;
    current_count_ = nullptr;
    current_list_ = nullptr;
    current_row_ = nullptr;
    current_title_ = nullptr;
    profile_buttons_[0] = nullptr;
    profile_buttons_[1] = nullptr;
    nearby_count_ = nullptr;
    device_list_ = nullptr;
    nearby_spinner_ = nullptr;
    scan_button_ = nullptr;
    scan_label_ = nullptr;
    device_rows_.clear();
    device_actions_.clear();
    device_titles_.clear();
    all_device_rows_.clear();
    all_device_actions_.clear();
    all_device_titles_.clear();
    device_addresses_.clear();
    device_seen_.clear();
    device_update_order_.clear();
    nearby_device_count_ = 0;
}

void View::SetBluetoothEnabled(bool enabled) {
    if (master_status_ != nullptr) {
        lv_label_set_text(master_status_, enabled ? I18n::T("蓝牙模式 · 已开启")
                                                  : I18n::T("蓝牙模式 · 已关闭"));
    }
    if (master_switch_ != nullptr) {
        if (enabled) {
            lv_obj_add_state(master_switch_, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(master_switch_, LV_STATE_CHECKED);
        }
    }
    if (local_panel_ != nullptr) {
        enabled
            ? lv_obj_add_flag(local_panel_, LV_OBJ_FLAG_HIDDEN)
            : lv_obj_remove_flag(local_panel_, LV_OBJ_FLAG_HIDDEN);
    }
    if (settings_panel_ != nullptr) {
        enabled
            ? lv_obj_remove_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN)
            : lv_obj_add_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void View::SetScanning(bool scanning) {
    if (device_list_ != nullptr && scanning) {
        lv_obj_remove_flag(device_list_, LV_OBJ_FLAG_HIDDEN);
    }
    if (scan_button_ != nullptr) {
        if (scanning) {
            lv_obj_add_state(scan_button_, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(scan_button_, LV_STATE_DISABLED);
        }
    }
    if (scan_label_ != nullptr) {
        lv_label_set_text(scan_label_, scanning ? I18n::T("扫描中…")
                                                : I18n::T("扫描音响"));
    }
    if (nearby_spinner_ != nullptr) {
        if (scanning) {
            lv_obj_remove_flag(nearby_spinner_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(nearby_spinner_);
        } else {
            lv_obj_add_flag(nearby_spinner_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void View::SetAudioProfile(bool connected, bool selected, bool call_profile) {
    ui::SetSegmentButtonSelected(profile_buttons_[0],
                                 selected && !call_profile);
    ui::SetSegmentButtonSelected(profile_buttons_[1],
                                 selected && call_profile);
    for (lv_obj_t* button : profile_buttons_) {
        if (button == nullptr) continue;
        if (connected) {
            lv_obj_remove_state(button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(button, LV_STATE_DISABLED);
        }
    }
}

void View::SetConnectingDevice(std::size_t index) {
    for (std::size_t row = 0; row < device_rows_.size(); ++row) {
        if (device_actions_[row] != nullptr) {
            lv_label_set_text(device_actions_[row],
                              row == index ? I18n::T("连接中...")
                                           : I18n::T("连接"));
        }
        if (device_rows_[row] != nullptr) {
            lv_obj_add_state(device_rows_[row], LV_STATE_DISABLED);
        }
    }
}

void View::ClearConnectingDevice() {
    for (std::size_t row = 0; row < device_rows_.size(); ++row) {
        if (device_actions_[row] != nullptr) {
            lv_label_set_text(device_actions_[row], I18n::T("连接"));
        }
        if (device_rows_[row] != nullptr) {
            lv_obj_remove_state(device_rows_[row], LV_STATE_DISABLED);
        }
    }
}

void View::ClearDevices() {
    nearby_device_count_ = 0;
    device_update_order_.clear();
    std::fill(device_seen_.begin(), device_seen_.end(), false);
    if (nearby_count_ != nullptr) {
        lv_label_set_text(nearby_count_, I18n::T("附近音响 · 0"));
    }
}

void View::FinishDeviceUpdate() {
    for (std::size_t index = device_addresses_.size(); index > 0; --index) {
        if (device_seen_[index - 1]) continue;
        if (all_device_rows_[index - 1] != nullptr) lv_obj_delete(all_device_rows_[index - 1]);
        device_addresses_.erase(device_addresses_.begin() + index - 1);
        device_seen_.erase(device_seen_.begin() + index - 1);
        // The all-row handles are maintained parallel to the address vectors.
        all_device_rows_.erase(all_device_rows_.begin() + index - 1);
        all_device_actions_.erase(all_device_actions_.begin() + index - 1);
        all_device_titles_.erase(all_device_titles_.begin() + index - 1);
    }

    std::vector<lv_obj_t*> ordered_rows;
    std::vector<lv_obj_t*> ordered_actions;
    std::vector<lv_obj_t*> ordered_titles;
    ordered_rows.reserve(device_update_order_.size());
    ordered_actions.reserve(device_update_order_.size());
    ordered_titles.reserve(device_update_order_.size());
    for (std::size_t index = 0; index < device_update_order_.size(); ++index) {
        auto found = std::find(device_addresses_.begin(), device_addresses_.end(),
                               device_update_order_[index]);
        if (found == device_addresses_.end()) continue;
        const std::size_t row_index = static_cast<std::size_t>(found - device_addresses_.begin());
        lv_obj_t* row = all_device_rows_[row_index];
        ordered_rows.push_back(row);
        ordered_actions.push_back(all_device_actions_[row_index]);
        ordered_titles.push_back(all_device_titles_[row_index]);
        if (device_list_ != nullptr) lv_obj_move_to_index(row, index + 1);
    }
    device_rows_.swap(ordered_rows);
    device_actions_.swap(ordered_actions);
    device_titles_.swap(ordered_titles);
    nearby_device_count_ = device_rows_.size();
    if (nearby_count_ != nullptr) {
        char count_text[64];
        std::snprintf(count_text, sizeof(count_text), I18n::T("附近音响 · %u"),
                      static_cast<unsigned>(nearby_device_count_));
        ui::SetLabelTextIfChanged(nearby_count_, count_text);
    }
    if (nearby_spinner_ != nullptr &&
        !lv_obj_has_flag(nearby_spinner_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(nearby_spinner_);
    }
}

std::size_t View::DeviceIndex(lv_obj_t* row) const {
    const auto found = std::find(device_rows_.begin(), device_rows_.end(), row);
    return found == device_rows_.end()
               ? static_cast<std::size_t>(-1)
               : static_cast<std::size_t>(found - device_rows_.begin());
}

void View::SetCurrentDevice(const char* address, const char* name) {
    if (current_list_ == nullptr) return;
    const bool connected =
        (address != nullptr && address[0] != '\0') ||
        (name != nullptr && name[0] != '\0');
    if (current_count_ != nullptr) {
        ui::SetLabelTextIfChanged(current_count_, connected
                                                       ? I18n::T("当前连接设备 · 1")
                                                       : I18n::T("当前连接设备 · 0"));
    }
    if (!connected) {
        lv_obj_add_flag(current_list_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(current_list_, LV_OBJ_FLAG_HIDDEN);
    const char* title = name != nullptr && name[0] != '\0' ? name : address;
    if (current_row_ == nullptr) {
        const auto row = ui::CreateCompactRow(
            current_list_, FONT_AWESOME_BLUETOOTH, title, nullptr,
            I18n::T("当前"), 72, true, false);
        current_row_ = row.root;
        current_title_ = row.title;
    } else {
        ui::SetLabelTextIfChanged(current_title_, title);
    }
}

lv_obj_t* View::AddDevice(const char* address, const char* name,
                          lv_event_cb_t callback, void* user_data) {
    if (device_list_ == nullptr) return nullptr;
    const std::string key = address != nullptr ? address : "";
    std::size_t index = device_addresses_.size();
    if (!key.empty()) {
        const auto found = std::find(device_addresses_.begin(), device_addresses_.end(), key);
        if (found != device_addresses_.end()) index = static_cast<std::size_t>(found - device_addresses_.begin());
    }
    if (index == device_addresses_.size()) {
        auto row = ui::CreateCompactRow(
            device_list_, FONT_AWESOME_BLUETOOTH,
            name != nullptr && name[0] != '\0' ? name : address,
            nullptr, I18n::T("连接"), 72, false, true, callback, user_data);
        all_device_rows_.push_back(row.root);
        all_device_actions_.push_back(row.trailing);
        all_device_titles_.push_back(row.title);
        device_addresses_.push_back(key.empty() ? "#" + std::to_string(device_addresses_.size()) : key);
        device_seen_.push_back(true);
        index = device_addresses_.size() - 1;
    } else {
        device_seen_[index] = true;
        ui::SetLabelTextIfChanged(all_device_titles_[index],
                                  name != nullptr && name[0] != '\0' ? name : address);
        ui::SetLabelTextIfChanged(all_device_actions_[index], I18n::T("连接"));
        lv_obj_remove_state(all_device_rows_[index], LV_STATE_DISABLED);
    }
    device_update_order_.push_back(device_addresses_[index]);
    return all_device_rows_[index];
}

}  // namespace agent_ui::bluetooth_settings_ui
