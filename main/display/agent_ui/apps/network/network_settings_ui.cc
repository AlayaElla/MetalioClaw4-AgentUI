#include "network_settings_ui.h"

#include <algorithm>
#include <cstdio>
#include <font_awesome.h>

#include "components/ui_components.h"
#include "core/fonts.h"
#include "core/theme.h"
#include "core/ui_utils.h"
#include "i18n.h"

namespace agent_ui::network_settings_ui {
namespace {

namespace controls = ui_components;
constexpr lv_opa_t kSelectedRowOpacity = 0x1F;

void DeleteCallbackIndex(lv_event_t* event) {
    delete static_cast<size_t*>(lv_event_get_user_data(event));
}

void UpdateRowStyle(ListRow& row, bool connected) {
    if (row.root == nullptr) return;
    const auto& colors = Theme::Get().colors();
    const uint32_t background = connected ? colors.accent : colors.background;
    const lv_opa_t opacity = connected ? kSelectedRowOpacity : LV_OPA_TRANSP;
    if (!row.style_ready || row.background_color != background) {
        lv_obj_set_style_bg_color(row.root, lv_color_hex(background), LV_PART_MAIN);
        row.background_color = background;
    }
    if (!row.style_ready || row.background_opacity != opacity) {
        lv_obj_set_style_bg_opa(row.root, opacity, LV_PART_MAIN);
        row.background_opacity = opacity;
    }
    row.connected = connected;
    row.style_ready = true;
}

ListRow& FindOrCreateRow(Handles& handles, lv_obj_t* parent,
                         std::vector<ListRow>& rows, const std::string& key,
                         const char* title, const char* trailing,
                         bool connected, bool clickable, size_t index,
                         lv_event_cb_t callback, size_t position) {
    auto found = std::find_if(rows.begin(), rows.end(), [&](const ListRow& row) {
        return row.key == key;
    });
    if (found == rows.end()) {
        ListRow row;
        row.key = key;
        if (clickable) {
            row.callback_index = new size_t(index + 1);
        }
        const auto parts = controls::CreateCompactRow(
            parent, key == "\001empty" ? nullptr : FONT_AWESOME_WIFI,
            title, nullptr, trailing, 68, connected, true, callback,
            row.callback_index);
        row.root = parts.root;
        row.title = parts.title;
        row.trailing = parts.trailing;
        row.connected = connected;
        if (row.callback_index != nullptr) {
            lv_obj_add_event_cb(row.root, DeleteCallbackIndex, LV_EVENT_DELETE,
                                row.callback_index);
        }
        rows.push_back(std::move(row));
        found = rows.end() - 1;
    } else {
        controls::SetLabelTextIfChanged(found->title, title);
        controls::SetLabelTextIfChanged(found->trailing, trailing != nullptr ? trailing : "");
        if (found->callback_index != nullptr) *found->callback_index = index + 1;
    }
    UpdateRowStyle(*found, connected);
    found->seen = true;
    if (parent != nullptr) lv_obj_move_to_index(found->root, position);
    (void)handles;
    return *found;
}

void RemoveStaleRows(std::vector<ListRow>& rows) {
    for (size_t index = rows.size(); index > 0; --index) {
        if (rows[index - 1].seen) {
            rows[index - 1].seen = false;
            continue;
        }
        if (rows[index - 1].root != nullptr) lv_obj_delete(rows[index - 1].root);
        rows.erase(rows.begin() + index - 1);
    }
}

void StyleList(lv_obj_t* list, int min_height) {
    lv_obj_set_height(list, min_height);
    lv_obj_set_style_min_height(list, min_height, LV_PART_MAIN);
    lv_obj_set_style_border_color(list,
                                  lv_color_hex(Theme::Get().colors().border),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(list, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    IgnoreSwipeBack(list, true);
}

}  // namespace

void ShowMode(Handles& handles, int selected) {
    for (int i = 0; i < 3; ++i) {
        controls::SetSegmentButtonSelected(handles.mode_buttons[i], i == selected);
        if (handles.mode_panels[i] == nullptr) continue;
        if (i == selected) {
            lv_obj_remove_flag(handles.mode_panels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(handles.mode_panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

Handles Build(lv_obj_t* parent, const Model& model,
              const Callbacks& callbacks) {
    Handles handles{};
    lv_obj_t* modes = controls::CreateSegment(parent, 70);
    handles.mode_buttons[0] = controls::AddSegmentButton(
        modes, FONT_AWESOME_WIFI, "Wi-Fi", false, callbacks.mode_selected,
        reinterpret_cast<void*>(static_cast<intptr_t>(0)));
    handles.mode_buttons[1] = controls::AddSegmentButton(
        modes, FONT_AWESOME_SIGNAL, I18n::T("内置卡"), false,
        callbacks.mode_selected,
        reinterpret_cast<void*>(static_cast<intptr_t>(1)));
    handles.mode_buttons[2] = controls::AddSegmentButton(
        modes, FONT_AWESOME_SD_CARD, I18n::T("外置卡"), false,
        callbacks.mode_selected,
        reinterpret_cast<void*>(static_cast<intptr_t>(2)));
    for (lv_obj_t* button : handles.mode_buttons) {
        if (button != nullptr) lv_obj_set_user_data(button, callbacks.owner);
    }

    lv_obj_t* wifi = controls::CreateContentPanel(parent, LV_SIZE_CONTENT, 4);
    handles.mode_panels[0] = wifi;
    auto saved_toolbar = controls::CreateToolbar(
        wifi, I18n::T("已保存网络 · 0"), nullptr);
    handles.saved_count = saved_toolbar.title;
    handles.saved_list = controls::CreateDividerList(wifi, 84);
    lv_obj_set_user_data(handles.saved_list, callbacks.owner);
    StyleList(handles.saved_list, 84);

    auto nearby_toolbar = controls::CreateToolbar(
        wifi, I18n::T("其他网络 · 0"), nullptr);
    handles.nearby_count = nearby_toolbar.title;

    handles.status = controls::AddValueLabel(nearby_toolbar.root, "", 390);

    handles.nearby_list = controls::CreateDividerList(wifi, 100);
    lv_obj_set_user_data(handles.nearby_list, callbacks.owner);
    StyleList(handles.nearby_list, 100);
    handles.nearby_spinner = lv_spinner_create(handles.nearby_list);
    lv_obj_set_size(handles.nearby_spinner, 64, 64);
    const auto& colors = Theme::Get().colors();
    lv_obj_set_style_arc_color(handles.nearby_spinner,
                               lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_arc_color(handles.nearby_spinner,
                               lv_color_hex(colors.accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(handles.nearby_spinner, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(handles.nearby_spinner, 7, LV_PART_INDICATOR);
    lv_obj_add_flag(handles.nearby_spinner, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(handles.nearby_spinner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(handles.nearby_spinner, LV_OBJ_FLAG_HIDDEN);

    auto scan = controls::AddWideActionButton(
        wifi, FONT_AWESOME_ARROWS_ROTATE, I18n::T("扫描网络"), callbacks.scan);
    handles.scan_button = scan.root;
    handles.scan_label = scan.label;
    lv_obj_set_user_data(handles.scan_button, callbacks.owner);

    lv_obj_t* internal = controls::CreateContentPanel(parent, 270);
    lv_obj_t* external = controls::CreateContentPanel(parent, 270);
    handles.mode_panels[1] = internal;
    handles.mode_panels[2] = external;
    lv_obj_t* internal_card = controls::CreateChoicePanel(
        internal, FONT_AWESOME_SIGNAL, I18n::T("内置卡"),
        nullptr, I18n::T("使用内置卡"),
        callbacks.internal_selected, nullptr,
        model.cellular && !model.external_slot);
    lv_obj_t* external_card = controls::CreateChoicePanel(
        external, FONT_AWESOME_SD_CARD, I18n::T("外置卡"),
        nullptr, I18n::T("使用外置卡"),
        callbacks.external_selected, nullptr,
        model.cellular && model.external_slot);
    if (internal_card != nullptr) lv_obj_set_user_data(internal_card, callbacks.owner);
    if (external_card != nullptr) lv_obj_set_user_data(external_card, callbacks.owner);

    ShowMode(handles,
             model.cellular ? (model.external_slot ? 2 : 1) : 0);
    return handles;
}

void RenderSaved(Handles& handles, const std::vector<SavedItem>& items,
                 lv_event_cb_t callback) {
    if (handles.saved_list == nullptr) return;
    lv_obj_set_height(
        handles.saved_list,
        std::max(84, static_cast<int>(std::min<size_t>(items.size(), 3)) * 68));
    if (handles.saved_count != nullptr) {
        char count_text[80];
        std::snprintf(count_text, sizeof(count_text),
                      I18n::T("已保存网络 · %u"),
                      static_cast<unsigned>(items.size()));
        lv_label_set_text(handles.saved_count, count_text);
    }
    if (items.empty()) {
        FindOrCreateRow(handles, handles.saved_list, handles.saved_rows,
                        "\001empty", I18n::T("暂无已连接过的 WiFi"), nullptr,
                        false, false, 0, nullptr, 0);
        RemoveStaleRows(handles.saved_rows);
        handles.saved_rendered = true;
        return;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        FindOrCreateRow(handles, handles.saved_list, handles.saved_rows,
                        items[i].ssid, items[i].ssid.c_str(),
                        items[i].is_connected ? I18n::T("当前") : I18n::T("连接"),
                        items[i].is_connected, true, i, callback, i);
    }
    RemoveStaleRows(handles.saved_rows);
    handles.saved_rendered = true;
}

void RenderNearby(Handles& handles, const std::vector<NearbyItem>& items,
                  bool scanning, bool scan_started, lv_event_cb_t callback) {
    if (handles.nearby_list == nullptr) return;
    if (scan_started) {
        lv_obj_remove_flag(handles.nearby_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(handles.nearby_list, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_height(
        handles.nearby_list,
        std::max(100, static_cast<int>(std::min<size_t>(items.size(), 3)) * 68));
    if (handles.nearby_count != nullptr) {
        char count_text[80];
        std::snprintf(count_text, sizeof(count_text),
                      I18n::T("其他网络 · %u"),
                      static_cast<unsigned>(items.size()));
        lv_label_set_text(handles.nearby_count, count_text);
    }
    if (items.empty()) {
        if (!scanning && scan_started) {
            FindOrCreateRow(handles, handles.nearby_list, handles.nearby_rows,
                            "\001empty", I18n::T("未发现网络"), nullptr,
                            false, false, 0, nullptr, 0);
        }
        RemoveStaleRows(handles.nearby_rows);
        handles.nearby_rendered = true;
        return;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        FindOrCreateRow(handles, handles.nearby_list, handles.nearby_rows,
                        items[i].ssid, items[i].ssid.c_str(), I18n::T("连接"),
                        false, true, i, callback, i);
    }
    RemoveStaleRows(handles.nearby_rows);
    if (handles.nearby_spinner != nullptr &&
        !lv_obj_has_flag(handles.nearby_spinner, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(handles.nearby_spinner);
    }
    handles.nearby_rendered = true;
}

void SetStatus(Handles& handles, const char*, uint32_t) {
    if (handles.status == nullptr) return;
    // Keep the nearby-network toolbar count-only; status events remain internal
    // so the existing scan and connection flows are otherwise unchanged.
    lv_label_set_text(handles.status, "");
}

void SetScanEnabled(Handles& handles, bool enabled) {
    if (handles.scan_button == nullptr) return;
    if (enabled) {
        lv_obj_remove_state(handles.scan_button, LV_STATE_DISABLED);
        if (handles.scan_label != nullptr) {
            lv_label_set_text(handles.scan_label, I18n::T("扫描网络"));
        }
    } else {
        lv_obj_add_state(handles.scan_button, LV_STATE_DISABLED);
        if (handles.scan_label != nullptr) lv_label_set_text(handles.scan_label, I18n::T("扫描中…"));
    }
}

void SetSpinnerVisible(Handles& handles, bool visible) {
    if (handles.nearby_spinner == nullptr) return;
    if (visible) {
        lv_obj_remove_flag(handles.nearby_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(handles.nearby_spinner);
    } else {
        lv_obj_add_flag(handles.nearby_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace agent_ui::network_settings_ui
