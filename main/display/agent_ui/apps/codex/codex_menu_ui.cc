#include "codex_menu_ui.h"

#include <cstdint>
#include <cstdio>
#include <font_awesome.h>
#include "core/fonts.h"
#include "core/theme.h"

namespace agent_ui::codex_menu_ui {
namespace {
namespace controls = ui_components;

lv_obj_t* Label(lv_obj_t* parent, const char* text, bool muted = false) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_height(label, fonts::Small()->line_height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, fonts::Small(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(muted ? Theme::Get().colors().muted
                                                       : Theme::Get().colors().text), LV_PART_MAIN);
    return label;
}

void Heading(lv_obj_t* parent, const char* text) {
    auto heading = controls::CreateToolbar(parent, text, nullptr);
    lv_obj_set_height(heading.root, 56);
    lv_obj_set_style_margin_bottom(heading.root, 12, LV_PART_MAIN);
}

void FitRow(const controls::CompactRowParts& row, int text_width) {
    if (row.title) {
        lv_obj_set_width(row.title, text_width);
        lv_obj_set_height(row.title, fonts::MediumBold()->line_height);
    }
    if (row.detail) {
        lv_obj_set_width(row.detail, text_width);
        lv_obj_set_height(row.detail, fonts::Small()->line_height);
    }
}
}  // namespace

Parts Build(lv_obj_t* root, const Callbacks& callbacks) {
    Parts p{};
    const auto drawer = controls::CreateRightDrawer(root, callbacks.dismiss);
    p.overlay = drawer.overlay;
    p.drawer = drawer.surface;
    p.content = drawer.content;
    p.tab_bar = drawer.tabs;
    for (auto& panel : p.panels) panel = controls::CreateContentPanel(p.content);

    auto* tasks = p.panels[0];
    Heading(tasks, "任务");
    auto create = controls::CreateCompactRow(tasks, FONT_AWESOME_PEN_TO_SQUARE,
        "新建任务", "创建一个空白 Codex 对话", nullptr,
        72, true, false, callbacks.new_task);
    controls::SetCompactRowIcon(create, controls::LineIcon::Plus, 32);
    create.trailing = controls::AddChevron(create.root);
    controls::StyleSettingsCard(create.root, true, false);
    lv_obj_set_style_border_opa(create.root, LV_OPA_TRANSP, LV_PART_MAIN);
    FitRow(create, 360);
    lv_obj_align(create.title, LV_ALIGN_TOP_LEFT, 54, 7);
    lv_obj_align(create.detail, LV_ALIGN_BOTTOM_LEFT, 54, -7);
    p.new_task = create.root;
    auto* list = controls::CreateContentPanel(tasks, LV_SIZE_CONTENT, 14);
    lv_obj_set_style_margin_top(list, 16, LV_PART_MAIN);
    for (int i = 0; i < 6; ++i) {
        char heading[24];
        std::snprintf(heading, sizeof(heading), "任务 %d", i + 1);
        p.tasks[i] = controls::CreateStatusCard(list, heading, callbacks.task,
            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_label_set_text(p.tasks[i].title, "待同步");
        lv_label_set_text(p.tasks[i].status, "未绑定");
        lv_obj_set_style_bg_color(p.tasks[i].dot,
            lv_color_hex(Theme::Get().colors().muted), LV_PART_MAIN);
    }

    auto* model = p.panels[1];
    Heading(model, "模型");
    p.model_context = Label(model, "当前任务待同步", true);
    lv_obj_set_style_margin_bottom(p.model_context, 12, LV_PART_MAIN);
    auto* current = Label(model, "当前模型");
    lv_obj_set_style_text_font(current, fonts::SmallBold(), LV_PART_MAIN);
    lv_obj_set_style_margin_bottom(current, 8, LV_PART_MAIN);
    p.model_dropdown = controls::CreateDropdownField(model, controls::LineIcon::Sparkles, callbacks.model);

    p.effort = controls::CreateChoiceSlider(model, "推理强度", callbacks.effort);
    lv_obj_set_style_margin_top(p.effort.root, 20, LV_PART_MAIN);

    auto fast = controls::CreateCompactRow(model, FONT_AWESOME_CLOUD_BOLT,
        "Fast 模式", "待同步", nullptr, 94, false, false);
    controls::SetCompactRowIcon(fast, controls::LineIcon::Zap, 36);
    controls::StyleSettingsCard(fast.root);
    lv_obj_set_style_bg_opa(fast.root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_margin_top(fast.root, 20, LV_PART_MAIN);
    FitRow(fast, 280);
    lv_obj_align(fast.title, LV_ALIGN_TOP_LEFT, 54, 14);
    lv_obj_align(fast.detail, LV_ALIGN_BOTTOM_LEFT, 54, -14);
    p.fast_detail = fast.detail;
    p.fast_switch = controls::AddSwitch(fast.root, false, callbacks.fast);
    lv_obj_set_size(p.fast_switch, 72, 38);

    auto* connection = p.panels[2];
    Heading(connection, "连接设置");
    auto* status = controls::CreateContentPanel(connection, LV_SIZE_CONTENT, 0);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(status, 12, LV_PART_MAIN);
    lv_obj_set_style_margin_bottom(status, 16, LV_PART_MAIN);
    p.connection_status_dot = lv_obj_create(status);
    lv_obj_remove_style_all(p.connection_status_dot);
    lv_obj_set_size(p.connection_status_dot, 10, 10);
    lv_obj_set_style_radius(p.connection_status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p.connection_status_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(p.connection_status_dot, LV_OBJ_FLAG_CLICKABLE);
    p.connection_status_text = Label(status, "等待连接状态");
    lv_obj_set_width(p.connection_status_text, 0);
    lv_obj_set_flex_grow(p.connection_status_text, 1);
    lv_obj_set_height(p.connection_status_text, LV_SIZE_CONTENT);
    lv_label_set_long_mode(p.connection_status_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(p.connection_status_text, fonts::SmallBold(), LV_PART_MAIN);
    SetConnectionStatus(p, false, "等待连接状态");
    auto* modes = controls::CreateSegment(connection, 72);
    p.modes[0] = controls::AddSegmentButton(modes, FONT_AWESOME_WIFI, "局域网", true,
        callbacks.mode, reinterpret_cast<void*>(0));
    p.modes[1] = controls::AddSegmentButton(modes, FONT_AWESOME_SIGNAL, "公网", false,
        callbacks.mode, reinterpret_cast<void*>(1));
    p.connection_panels[0] = controls::CreateContentPanel(connection, 96);
    lv_obj_set_style_margin_top(p.connection_panels[0], 16, LV_PART_MAIN);
    auto device = controls::CreateCompactRow(p.connection_panels[0], FONT_AWESOME_LINK,
        "正在自动发现 PC 服务…", "局域网", nullptr, 96, false, false);
    controls::SetCompactRowIcon(device, controls::LineIcon::Monitor, 38);
    device.trailing = controls::AddChevron(device.root);
    controls::StyleSettingsCard(device.root);
    lv_obj_set_style_bg_opa(device.root, LV_OPA_TRANSP, LV_PART_MAIN);
    FitRow(device, 350);
    lv_obj_align(device.title, LV_ALIGN_TOP_LEFT, 54, 14);
    lv_obj_align(device.detail, LV_ALIGN_BOTTOM_LEFT, 54, -14);
    p.discovery_name = device.title;
    p.connection_panels[1] = controls::CreateContentPanel(connection, LV_SIZE_CONTENT, 8);
    lv_obj_set_style_margin_top(p.connection_panels[1], 16, LV_PART_MAIN);
    Label(p.connection_panels[1], "公网 IP 地址");
    p.remote_ip = controls::CreateTextField(p.connection_panels[1], "例如 203.0.113.10", callbacks.connection_changed);
    lv_obj_add_flag(p.connection_panels[1], LV_OBJ_FLAG_HIDDEN);
    auto* token = controls::CreateContentPanel(connection, LV_SIZE_CONTENT, 8);
    lv_obj_set_style_margin_top(token, 24, LV_PART_MAIN);
    Label(token, "认证 Token");
    p.token = controls::CreateTextField(token, "输入认证 Token", callbacks.connection_changed);
    lv_textarea_set_password_mode(p.token, true);
    p.connect = controls::AddWideActionButton(connection, FONT_AWESOME_LINK, "连接", callbacks.connect);
    lv_obj_set_style_margin_top(p.connect.root, 18, LV_PART_MAIN);
    auto* display_heading = controls::CreateSectionHeading(connection, "显示设置");
    lv_obj_set_style_margin_top(display_heading, 28, LV_PART_MAIN);
    lv_obj_set_style_margin_bottom(display_heading, 12, LV_PART_MAIN);
    auto ring = controls::CreateCompactRow(connection, FONT_AWESOME_BRIGHTNESS,
        "屏幕状态光圈", nullptr, nullptr, 84, false, false);
    controls::StyleSettingsCard(ring.root);
    lv_obj_set_style_bg_opa(ring.root, LV_OPA_TRANSP, LV_PART_MAIN);
    FitRow(ring, 280);
    p.ring_switch = controls::AddSwitch(ring.root, true, callbacks.ring);
    lv_obj_set_size(p.ring_switch, 72, 38);

    auto* notification_heading = controls::CreateSectionHeading(connection, "通知声音");
    lv_obj_set_style_margin_top(notification_heading, 28, LV_PART_MAIN);
    lv_obj_set_style_margin_bottom(notification_heading, 12, LV_PART_MAIN);
    auto notification = controls::CreateCompactRow(connection, FONT_AWESOME_BELL,
        "Codex 任务提醒", "审批、提问和完成时提示", nullptr, 84, false, false);
    controls::StyleSettingsCard(notification.root);
    lv_obj_set_style_bg_opa(notification.root, LV_OPA_TRANSP, LV_PART_MAIN);
    FitRow(notification, 280);
    lv_obj_align(notification.title, LV_ALIGN_TOP_LEFT, 54, 10);
    lv_obj_align(notification.detail, LV_ALIGN_BOTTOM_LEFT, 54, -10);
    p.notification_switch = controls::AddSwitch(notification.root, true, callbacks.notification_enabled);
    lv_obj_set_size(p.notification_switch, 72, 38);

    const controls::LineIcon icons[] = {controls::LineIcon::Tasks, controls::LineIcon::Sliders, controls::LineIcon::Link};
    const char* labels[] = {"任务", "模型", "连接"};
    for (int i = 0; i < 3; ++i) p.tabs[i] = controls::AddDrawerTab(p.tab_bar, icons[i], labels[i],
        callbacks.tab, reinterpret_cast<void*>(static_cast<uintptr_t>(i))).root;
    SelectTab(p, 0);
    return p;
}

void SelectTab(Parts& p, int index) {
    if (index < 0 || index >= 3 || index == p.selected_tab) return;
    for (int i = 0; i < 3; ++i) {
        if (p.selected_tab >= 0 && i != index && i != p.selected_tab) continue;
        controls::SetDrawerTabSelected(p.tabs[i], i == index);
        if (i == index) lv_obj_remove_flag(p.panels[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(p.panels[i], LV_OBJ_FLAG_HIDDEN);
    }
    p.selected_tab = index;
    lv_obj_update_layout(p.content);
    if (lv_obj_get_scroll_y(p.content) != 0) lv_obj_scroll_to_y(p.content, 0, LV_ANIM_OFF);
}
namespace {
std::string RefreshKey(const codex_menu::State& state, int tab, bool pending, bool voice_busy) {
    std::string key;
    const auto add = [&](const std::string& value) { key += std::to_string(value.size()) + ":" + value; };
    const auto number = [&](int value) { add(std::to_string(value)); };
    number(pending); number(voice_busy); number(state.connected); number(state.selected_slot);
    if (tab == 0) {
        number(state.can_new_task); number(state.can_select_task);
        for (const auto& slot : state.slots) {
            add(slot.host_id); add(slot.thread_id); add(slot.title); number(static_cast<int>(slot.state));
        }
    } else {
        add(state.stream_id);
        add(state.draft_request_id); add(state.draft_status); add(state.draft_settings_error);
        number(state.draft_settings_loading); number(state.draft_submitted);
        number(state.can_set_model); number(state.can_set_effort); number(state.can_set_fast);
        if (const auto* target = codex_menu::SettingsTarget(state)) {
            const auto& slot = *target;
            add(slot.host_id); add(slot.thread_id); add(slot.title); number(slot.synced);
            add(slot.model); add(slot.effort); number(slot.model_ready); number(slot.effort_ready);
            number(slot.has_fast); number(slot.fast); number(slot.fast_ready);
        }
        for (const auto& model : state.models) {
            add(model.id); add(model.label); number(model.fast_supported);
            for (const auto& effort : model.efforts) { add(effort.id); add(effort.label); }
        }
    }
    return key;
}
}  // namespace

void Refresh(Parts& p, const codex_menu::State& state, bool pending, bool voice_busy, bool force) {
    if (lv_obj_has_flag(p.overlay, LV_OBJ_FLAG_HIDDEN) || p.selected_tab == 2) return;
    const auto key = RefreshKey(state, p.selected_tab, pending, voice_busy);
    if (!force && p.rendered_tabs[p.selected_tab] == key) return;
    p.rendered_tabs[p.selected_tab] = key;
    const auto& colors = Theme::Get().colors();
    if (p.selected_tab == 0) {
        if (p.new_task != nullptr) {
            if (!state.can_new_task || pending || voice_busy) lv_obj_add_state(p.new_task, LV_STATE_DISABLED);
            else lv_obj_remove_state(p.new_task, LV_STATE_DISABLED);
        }
        for (int i = 0; i < 6; ++i) {
            if (p.tasks[i].root == nullptr) continue;
            const auto& slot = state.slots[i];
            const bool selected = state.selected_slot == i;
            controls::StyleSettingsCard(p.tasks[i].root, selected, false);
            if (!state.can_select_task || pending || voice_busy || !codex_menu::IsBound(slot)) lv_obj_add_state(p.tasks[i].root, LV_STATE_DISABLED);
            else lv_obj_remove_state(p.tasks[i].root, LV_STATE_DISABLED);
            const auto& card = p.tasks[i];
            controls::SetLabelTextIfChanged(card.title, codex_menu::IsBound(slot) ? slot.title.c_str() : "待同步");
            controls::SetLabelTextIfChanged(card.status, pending ? "同步中" : codex_menu::StateLabel(slot.state).c_str());
            const uint32_t color = slot.state == codex_menu::SlotState::Error ? colors.danger
                : slot.state == codex_menu::SlotState::Waiting ? colors.warning
                : slot.state == codex_menu::SlotState::Working ? colors.accent : colors.muted;
            lv_obj_set_style_text_color(card.status, lv_color_hex(color), LV_PART_MAIN);
            lv_obj_set_style_bg_color(card.dot, lv_color_hex(color), LV_PART_MAIN);
            if (selected) lv_obj_remove_flag(card.check, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(card.check, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    const int selected = state.selected_slot;
    const auto* slot = codex_menu::SettingsTarget(state);
    const bool draft = slot == &state.draft_settings;
    const auto* model = slot ? codex_menu::FindModel(state, slot->model) : nullptr;
    const bool data_ready = slot && state.connected && slot->synced;
    const bool helper_busy = draft && state.draft_settings_loading;
    const bool editable = data_ready && !draft && !pending && !voice_busy && !helper_busy;
    if (p.model_context) {
        const auto context = draft ? (!state.draft_settings_error.empty() ? state.draft_settings_error : state.draft_status == "preparing" ? std::string("正在准备新任务…") :
            std::string("新任务使用默认参数，发送后可调整")) :
            data_ready ? (selected >= 0 ? "任务 " + std::to_string(selected + 1) : std::string("当前任务")) +
                " · " + slot->title : "当前任务待同步";
        controls::SetLabelTextIfChanged(p.model_context, context.c_str());
    }
    if (p.fast_detail) controls::SetLabelTextIfChanged(p.fast_detail,
        draft ? "使用默认参数" : data_ready && slot->fast_ready && slot->has_fast ? (slot->fast ? "已开启" : "已关闭") : "待同步");
    if (p.model_dropdown) {
        std::string options;
        uint32_t model_index = 0;
        for (size_t i = 0; i < state.models.size(); ++i) {
            if (!options.empty()) options += "\n";
            options += state.models[i].label;
            if (slot && state.models[i].id == slot->model) model_index = static_cast<uint32_t>(i);
        }
        if (!data_ready || !slot->model_ready || state.models.empty()) {
            options = draft ? "使用默认参数" : "待同步";
            model_index = 0;
        }
        if (options != lv_dropdown_get_options(p.model_dropdown)) {
            lv_dropdown_close(p.model_dropdown);
            lv_dropdown_set_options(p.model_dropdown, options.c_str());
        }
        if (!editable || !slot->model_ready || state.models.empty() || !state.can_set_model) {
            lv_dropdown_close(p.model_dropdown);
            lv_obj_add_state(p.model_dropdown, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(p.model_dropdown, LV_STATE_DISABLED);
        }
        lv_dropdown_set_text(p.model_dropdown, data_ready && slot->model_ready && !model && !state.models.empty() ? "请选择模型" : nullptr);
        if (!lv_dropdown_is_open(p.model_dropdown) &&
            lv_dropdown_get_selected(p.model_dropdown) != model_index) {
            lv_dropdown_set_selected(p.model_dropdown, model_index);
        }
    }
    if (p.effort.slider) {
        std::vector<std::string> options;
        std::string context = data_ready && slot->effort_ready && model ? state.stream_id + "\n" + slot->host_id +
            "\n" + (draft ? state.draft_request_id : slot->thread_id) + "\n" + model->id : "";
        uint32_t value = UINT32_MAX;
        if (data_ready && slot->effort_ready && model) for (size_t i = 0; i < model->efforts.size(); ++i) {
            options.push_back(model->efforts[i].label);
            context += "\n" + model->efforts[i].id;
            if (model->efforts[i].id == slot->effort) value = static_cast<uint32_t>(i);
        }
        controls::UpdateChoiceSlider(p.effort, options, value,
                                    editable && slot->effort_ready && model && !model->efforts.empty() && state.can_set_effort,
                                    pending, context);
    }
    if (p.fast_switch) {
        if (data_ready && slot->fast_ready && slot->has_fast && slot->fast) lv_obj_add_state(p.fast_switch, LV_STATE_CHECKED); else lv_obj_remove_state(p.fast_switch, LV_STATE_CHECKED);
        if (!editable || !model || !slot || !slot->fast_ready || !slot->has_fast || !state.can_set_fast) lv_obj_add_state(p.fast_switch, LV_STATE_DISABLED);
        else lv_obj_remove_state(p.fast_switch, LV_STATE_DISABLED);
    }
}

void SetConnectionStatus(Parts& p, bool connected, const char* text) {
    if (p.connection_status_text) {
        controls::SetLabelTextIfChanged(p.connection_status_text, text ? text : "等待连接状态");
    }
    if (p.connection_status_dot) {
        const auto& colors = Theme::Get().colors();
        lv_obj_set_style_bg_color(p.connection_status_dot,
            lv_color_hex(connected ? colors.accent : colors.danger), LV_PART_MAIN);
    }
}

void SetNotificationSettings(Parts& p, bool enabled) {
    if (p.notification_switch != nullptr) {
        if (enabled) lv_obj_add_state(p.notification_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(p.notification_switch, LV_STATE_CHECKED);
    }
}

}  // namespace agent_ui::codex_menu_ui
