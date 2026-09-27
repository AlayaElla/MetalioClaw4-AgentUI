#include "status_bar.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <utility>

#include <font_awesome.h>

#include "application.h"
#include "ai/ai_availability.h"
#include "apps/bluetooth/bluetooth_adapter.h"
#include "apps/home/home_renderer.h"
#include "board.h"
#include "fonts.h"
#include "theme.h"
#include "dual_network_board.h"
#include "settings.h"
#include "status_signal_assets.h"

namespace agent_ui {
namespace {

const lv_image_dsc_t* SelectNetworkAsset(NetworkMode mode, const char* icon) {
    using namespace status_signal_assets;
    if (mode == NetworkMode::Wifi) {
        if (icon == nullptr || icon[0] == '\0') return &kWifi0;
        if (std::strcmp(icon, FONT_AWESOME_WIFI_SLASH) == 0) return &kDisconnected;
        if (std::strcmp(icon, FONT_AWESOME_WIFI_WEAK) == 0) return &kWifi1;
        if (std::strcmp(icon, FONT_AWESOME_WIFI_FAIR) == 0) return &kWifi2;
        if (std::strcmp(icon, FONT_AWESOME_WIFI) == 0) return &kWifi3;
        return &kWifi0;
    }

    if (icon == nullptr || icon[0] == '\0' ||
        std::strcmp(icon, FONT_AWESOME_SIGNAL_OFF) == 0) {
        return &kCellular0;
    }
    if (std::strcmp(icon, FONT_AWESOME_SIGNAL_WEAK) == 0) return &kCellular1;
    if (std::strcmp(icon, FONT_AWESOME_SIGNAL_FAIR) == 0) return &kCellular2;
    if (std::strcmp(icon, FONT_AWESOME_SIGNAL_GOOD) == 0) return &kCellular3;
    if (std::strcmp(icon, FONT_AWESOME_SIGNAL_STRONG) == 0) return &kCellular4;
    return &kCellular0;
}

int BatteryCellCount(bool has_battery, int level) {
    if (!has_battery || level < 20) return 0;
    if (level < 50) return 1;
    if (level < 80) return 2;
    return 3;
}

void SetA8Color(lv_obj_t* image, uint32_t color) {
    lv_obj_set_style_image_recolor(image, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, LV_PART_MAIN);
}

// The words behind the face. Home hides the mini face and has no caption of its
// own, so this is the only place its state is spelled out.
const char* AgentStateText(AgentState state) {
    switch (state) {
        case AgentState::Connecting:
            return "连接中";
        case AgentState::Listening:
            return "聆听中";
        case AgentState::Answering:
            return "说话中";
        case AgentState::Error:
            return "异常";
        case AgentState::Idle:
        default:
            return "待机";
    }
}

}  // namespace

void StatusBar::AgentFaceClicked(lv_event_t* event) {
    auto* self = static_cast<StatusBar*>(lv_event_get_user_data(event));
    if (self == nullptr || !ai::Availability::Get().IsAvailable()) return;
    self->SetReplyCaption(nullptr);
    // The callback is runtime-owned, so the shared top control does not
    // depend on whichever application screen is currently mounted.
    if (self->agent_tap_callback_) self->agent_tap_callback_();
}

StatusBar& StatusBar::Get() {
    static StatusBar instance;
    return instance;
}

void StatusBar::Initialize() {
    if (root_ == nullptr) Create();
    // The display is created from inside Board::GetInstance(). Refreshing
    // here would recursively request the board while its singleton is still
    // under construction. SetVisible(true) performs the first real refresh.
}

void StatusBar::Create() {
    const auto& colors = Theme::Get().colors();
    root_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, metrics::kDisplaySize, metrics::kStatusBarHeight);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(colors.background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);

    left_cluster_ = lv_obj_create(root_);
    lv_obj_remove_style_all(left_cluster_);
    lv_obj_set_size(left_cluster_, 174, 34);
    lv_obj_align(left_cluster_, LV_ALIGN_LEFT_MID, 34, 0);
    lv_obj_set_flex_flow(left_cluster_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_cluster_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_cluster_, 12, LV_PART_MAIN);
    lv_obj_remove_flag(left_cluster_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(left_cluster_, LV_OBJ_FLAG_CLICKABLE);

    bluetooth_icon_ = lv_label_create(left_cluster_);
    lv_label_set_text(bluetooth_icon_, FONT_AWESOME_BLUETOOTH);
    lv_obj_set_style_text_font(bluetooth_icon_, fonts::IconLarge(), LV_PART_MAIN);
    lv_obj_set_style_text_color(bluetooth_icon_, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_width(bluetooth_icon_, 28);
    lv_obj_set_style_text_align(bluetooth_icon_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_translate_y(bluetooth_icon_, -4, LV_PART_MAIN);
    lv_obj_add_flag(bluetooth_icon_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(bluetooth_icon_, LV_OBJ_FLAG_CLICKABLE);

    network_icon_ = lv_image_create(left_cluster_);
    lv_image_set_src(network_icon_, &status_signal_assets::kWifi0);
    lv_obj_set_size(network_icon_, 28, 28);
    lv_obj_set_style_translate_y(network_icon_, -4, LV_PART_MAIN);
    SetA8Color(network_icon_, colors.text);
    lv_obj_remove_flag(network_icon_, LV_OBJ_FLAG_CLICKABLE);

    time_label_ = lv_label_create(left_cluster_);
    lv_obj_set_style_text_font(time_label_, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(time_label_, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_width(time_label_, 120);
    lv_obj_set_style_text_align(time_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);

    agent_cluster_ = lv_obj_create(root_);
    lv_obj_remove_style_all(agent_cluster_);
    lv_obj_set_size(agent_cluster_, 360, metrics::kStatusBarHeight);
    lv_obj_align(agent_cluster_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(agent_cluster_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(agent_cluster_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(agent_cluster_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    agent_face_slot_ = lv_obj_create(agent_cluster_);
    lv_obj_remove_style_all(agent_face_slot_);
    lv_obj_set_size(agent_face_slot_, 126, 60);
    lv_obj_set_pos(agent_face_slot_, 117, 1);
    lv_obj_add_flag(agent_face_slot_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(agent_face_slot_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(agent_face_slot_, AgentFaceClicked, LV_EVENT_CLICKED,
                        this);
    agent_expression_ = std::make_unique<ExpressionPlayer>(
        agent_face_slot_, nullptr, 126, 60, 4);

    // A tiny state dot remains available to distinguish disabled AI from an
    // ordinary idle expression without introducing a text-only status mode.
    agent_dot_ = lv_obj_create(agent_face_slot_);
    lv_obj_remove_style_all(agent_dot_);
    lv_obj_set_size(agent_dot_, 6, 6);
    lv_obj_set_style_radius(agent_dot_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(agent_dot_, lv_color_hex(colors.muted), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(agent_dot_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(agent_dot_, 116, 4);

    reply_clip_ = lv_obj_create(agent_cluster_);
    lv_obj_remove_style_all(reply_clip_);
    lv_obj_remove_flag(reply_clip_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(reply_clip_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(reply_clip_, 140, 9);
    lv_obj_set_size(reply_clip_, 220, 44);
    agent_label_ = lv_label_create(reply_clip_);
    lv_label_set_text(agent_label_, "");
    lv_obj_set_style_text_font(agent_label_, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(agent_label_, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_size(agent_label_, 220, 44);
    lv_obj_set_pos(agent_label_, 0, 0);
    lv_label_set_long_mode(agent_label_, LV_LABEL_LONG_CLIP);
    lv_obj_add_flag(agent_label_, LV_OBJ_FLAG_HIDDEN);

    right_cluster_ = lv_obj_create(root_);
    lv_obj_remove_style_all(right_cluster_);
    lv_obj_set_size(right_cluster_, 119, metrics::kStatusBarHeight);
    lv_obj_align(right_cluster_, LV_ALIGN_RIGHT_MID, -34, 0);
    lv_obj_set_flex_flow(right_cluster_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_cluster_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(right_cluster_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(right_cluster_, LV_OBJ_FLAG_CLICKABLE);

    battery_group_ = lv_obj_create(right_cluster_);
    lv_obj_remove_style_all(battery_group_);
    lv_obj_set_size(battery_group_, 119, 34);
    lv_obj_set_flex_flow(battery_group_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_group_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(battery_group_, 9, LV_PART_MAIN);
    lv_obj_remove_flag(battery_group_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(battery_group_, LV_OBJ_FLAG_CLICKABLE);

    battery_label_ = lv_label_create(battery_group_);
    StyleLabel(battery_label_);
    lv_obj_set_style_text_font(battery_label_, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_width(battery_label_, 68);
    lv_obj_set_style_text_align(battery_label_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    battery_icon_ = lv_obj_create(battery_group_);
    lv_obj_remove_style_all(battery_icon_);
    lv_obj_set_size(battery_icon_, 42, 28);
    lv_obj_set_style_translate_y(battery_icon_, -3, LV_PART_MAIN);
    lv_obj_remove_flag(battery_icon_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(battery_icon_, LV_OBJ_FLAG_CLICKABLE);

    battery_outline_ = lv_obj_create(battery_icon_);
    lv_obj_remove_style_all(battery_outline_);
    lv_obj_set_size(battery_outline_, 36, 24);
    lv_obj_set_pos(battery_outline_, 0, 2);
    lv_obj_set_style_radius(battery_outline_, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(battery_outline_, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(battery_outline_, lv_color_hex(colors.text),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(battery_outline_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(battery_outline_, LV_OPA_TRANSP, LV_PART_MAIN);

    for (size_t index = 0; index < battery_cells_.size(); ++index) {
        battery_cells_[index] = lv_obj_create(battery_icon_);
        lv_obj_remove_style_all(battery_cells_[index]);
        lv_obj_set_size(battery_cells_[index], 7, 14);
        lv_obj_set_pos(battery_cells_[index], 5 + static_cast<int>(index) * 10, 7);
        lv_obj_set_style_radius(battery_cells_[index], 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            battery_cells_[index], lv_color_hex(colors.text), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(battery_cells_[index], LV_OPA_20, LV_PART_MAIN);
        lv_obj_remove_flag(battery_cells_[index], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(battery_cells_[index], LV_OBJ_FLAG_CLICKABLE);
    }

    battery_tip_ = lv_obj_create(battery_icon_);
    lv_obj_remove_style_all(battery_tip_);
    lv_obj_set_size(battery_tip_, 4, 10);
    lv_obj_set_pos(battery_tip_, 37, 9);
    lv_obj_set_style_radius(battery_tip_, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_tip_, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(battery_tip_, LV_OPA_COVER, LV_PART_MAIN);

    battery_bolt_outline_ = lv_image_create(battery_icon_);
    lv_image_set_src(battery_bolt_outline_, &status_signal_assets::kBatteryBoltOutline);
    lv_obj_set_pos(battery_bolt_outline_, 7, 0);
    SetA8Color(battery_bolt_outline_, colors.background);
    lv_obj_add_flag(battery_bolt_outline_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(battery_bolt_outline_, LV_OBJ_FLAG_CLICKABLE);

    battery_bolt_fill_ = lv_image_create(battery_icon_);
    lv_image_set_src(battery_bolt_fill_, &status_signal_assets::kBatteryBoltFill);
    lv_obj_set_pos(battery_bolt_fill_, 7, 0);
    SetA8Color(battery_bolt_fill_, colors.accent);
    lv_obj_add_flag(battery_bolt_fill_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(battery_bolt_fill_, LV_OBJ_FLAG_CLICKABLE);

    timer_ = lv_timer_create(TimerCallback, 1000, this);
    lv_obj_add_event_cb(root_, DeletedCallback, LV_EVENT_DELETE, this);
}

NetworkMode StatusBar::ReadNetworkMode() const {
    const NetworkType type = DualNetworkBoard::LoadNetworkTypeFromSettings(1);
    if (type == NetworkType::WIFI) return NetworkMode::Wifi;

    Settings settings("network", true);
    return settings.GetInt("sim_slot", 0) == 1 ? NetworkMode::InternalSim
                                                : NetworkMode::ExternalSim;
}

void StatusBar::Refresh(bool force) {
    if (root_ == nullptr) return;

    const auto& colors = Theme::Get().colors();
    const bool available = ai::Availability::Get().IsAvailable();
    if (force || available != ai_available_) {
        ai_available_ = available;
        if (agent_face_slot_ != nullptr) {
            if (available && !lock_screen_mode_) {
                lv_obj_remove_flag(agent_face_slot_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(agent_face_slot_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (!available) ClearReplyCaption();
        UpdateAgentPresentation();
    }
    const bool bluetooth_enabled = bluetooth::Adapter::Get().IsEnabled();
    const bool bluetooth_connected = bluetooth_enabled &&
                                     bluetooth::Adapter::Get().IsConnected();
    lv_obj_set_width(left_cluster_, bluetooth_enabled ? 214 : 174);
    if (force || last_bluetooth_enabled_ != bluetooth_enabled ||
        last_bluetooth_connected_ != bluetooth_connected) {
        last_bluetooth_enabled_ = bluetooth_enabled;
        last_bluetooth_connected_ = bluetooth_connected;
        if (bluetooth_enabled) {
            lv_obj_remove_flag(bluetooth_icon_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bluetooth_icon_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_style_text_color(bluetooth_icon_, lv_color_hex(colors.text),
                                LV_PART_MAIN);
    network_mode_ = ReadNetworkMode();
    const char* network_icon = Board::GetInstance().GetNetworkStateIcon();
    const lv_image_dsc_t* network_asset = SelectNetworkAsset(network_mode_, network_icon);
    if (force || last_network_asset_ != network_asset) {
        last_network_asset_ = network_asset;
        lv_image_set_src(network_icon_, network_asset);
    }
    lv_obj_set_style_bg_color(root_, lv_color_hex(colors.background), LV_PART_MAIN);
    SetA8Color(network_icon_, colors.text);

    char time_buffer[16] = "--:--";
    const time_t now = time(nullptr);
    struct tm local = {};
    if (localtime_r(&now, &local) != nullptr && local.tm_year >= 125) {
        strftime(time_buffer, sizeof(time_buffer), "%H:%M", &local);
    }
    const auto& app = Application::GetInstance();
    std::string center_text;
    const bool has_activation = app.HasPendingActivation();
    if (has_activation) {
        center_text = "验证码: " + app.GetPendingActivationCode();
    }
    if (force || last_time_text_ != time_buffer) {
        last_time_text_ = time_buffer;
        lv_label_set_text(time_label_, time_buffer);
    }
    lv_obj_set_style_text_color(
        time_label_, lv_color_hex(colors.text), LV_PART_MAIN);
    // Theme changes must repaint the already-rendered agent state as well as
    // the network, clock, and battery colors refreshed below.
    if (force) SetAgentState(agent_state_);
    if (force || last_center_text_ != center_text) {
        last_center_text_ = center_text;
        UpdateAgentPresentation();
    }

    int level = 0;
    bool charging = false;
    bool discharging = false;
    const bool has_battery = Board::GetInstance().GetBatteryLevel(
        level, charging, discharging);
    home::Renderer::UpdateBattery(has_battery, level, charging);
    char battery_text[12] = "--%";
    if (has_battery) {
        level = std::clamp(level, 0, 100);
        std::snprintf(battery_text, sizeof(battery_text), "%d%%", level);
    }
    if (force || last_battery_text_ != battery_text) {
        last_battery_text_ = battery_text;
        lv_label_set_text(battery_label_, battery_text);
    }
    const uint32_t battery_color = has_battery && !charging && level < 20
                                       ? colors.danger
                                       : (charging ? colors.accent : colors.text);
    const uint32_t outline_color = has_battery ? battery_color : colors.muted;
    lv_obj_set_style_text_color(
        battery_label_, lv_color_hex(outline_color), LV_PART_MAIN);
    lv_obj_set_style_border_color(
        battery_outline_, lv_color_hex(outline_color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        battery_tip_, lv_color_hex(outline_color), LV_PART_MAIN);
    const int active_cells = BatteryCellCount(has_battery, level);
    for (size_t index = 0; index < battery_cells_.size(); ++index) {
        lv_obj_set_style_bg_color(
            battery_cells_[index], lv_color_hex(outline_color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(
            battery_cells_[index], static_cast<int>(index) < active_cells
                                       ? LV_OPA_COVER
                                       : LV_OPA_20,
            LV_PART_MAIN);
    }
    SetA8Color(battery_bolt_outline_, colors.background);
    SetA8Color(battery_bolt_fill_, colors.accent);
    if (charging) {
        lv_obj_remove_flag(battery_bolt_outline_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(battery_bolt_fill_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(battery_bolt_outline_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_bolt_fill_, LV_OBJ_FLAG_HIDDEN);
    }
}

void StatusBar::SetAgentState(AgentState state) {
    agent_state_ = state;
    if (agent_dot_ == nullptr || agent_label_ == nullptr) return;
    const auto& colors = Theme::Get().colors();
    const bool active = state != AgentState::Idle;
    const uint32_t color = active ? colors.accent : colors.muted;
    lv_obj_set_style_bg_color(agent_dot_, lv_color_hex(color), LV_PART_MAIN);
    if (agent_expression_) agent_expression_->SetState(state);
    if (state == AgentState::Listening || state == AgentState::Connecting) {
        ClearReplyCaption();
    }
    UpdateAgentPresentation();
}

void StatusBar::SetReplyCaption(const char* caption) {
    const std::string next = caption != nullptr ? caption : "";
    ClearReplyCaption();
    reply_caption_ = next;
    if (reply_caption_.empty()) {
        ClearReplyCaption();
        UpdateAgentPresentation();
    } else {
        agent_state_ = AgentState::Answering;
        if (agent_expression_) agent_expression_->SetState(agent_state_);
        UpdateAgentPresentation();
    }
}

void StatusBar::SetAgentTapCallback(AgentTapCallback callback) {
    agent_tap_callback_ = std::move(callback);
}

void StatusBar::ClearReplyCaption() {
    if (reply_finish_timer_) {
        lv_timer_delete(reply_finish_timer_);
        reply_finish_timer_ = nullptr;
    }
    if (agent_label_) lv_anim_delete(agent_label_, SetObjectTranslateX);
    reply_caption_.clear();
    reply_wide_ = false;
    reply_presented_ = false;
    if (agent_label_ != nullptr) {
        lv_label_set_text(agent_label_, "");
        lv_obj_add_flag(agent_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (battery_group_ != nullptr) {
        lv_anim_delete(battery_group_, SetAgentClusterTranslateY);
        lv_obj_set_style_translate_y(battery_group_, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(battery_group_, LV_OPA_COVER, LV_PART_MAIN);
    }
}

void StatusBar::SetObjectTranslateX(void* object, int32_t value) {
    auto* target = static_cast<lv_obj_t*>(object);
    if (target != nullptr && lv_obj_is_valid(target)) {
        lv_obj_set_style_translate_x(target, value, LV_PART_MAIN);
    }
}

void StatusBar::AnimateAgentFace(int32_t target, uint32_t duration_ms) {
    if (agent_face_slot_ == nullptr || !lv_obj_is_valid(agent_face_slot_)) return;
    lv_anim_delete(agent_face_slot_, SetObjectTranslateX);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, agent_face_slot_);
    lv_anim_set_exec_cb(&animation, SetObjectTranslateX);
    lv_anim_set_values(&animation,
        lv_obj_get_style_translate_x(agent_face_slot_, LV_PART_MAIN), target);
    lv_anim_set_duration(&animation, duration_ms);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_start(&animation);
}

void StatusBar::UpdateAgentPresentation() {
    if (!agent_face_slot_ || !agent_label_ || !reply_clip_) return;
    const bool show_caption = !reply_caption_.empty() && !lock_screen_mode_ && ai_available_;
    if (home_active_ || lock_screen_mode_ || !ai_available_) lv_obj_add_flag(agent_face_slot_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(agent_face_slot_, LV_OBJ_FLAG_HIDDEN);
    if (show_caption) {
        lv_obj_remove_flag(agent_cluster_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(agent_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(agent_label_, LV_TEXT_ALIGN_LEFT,
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(agent_label_,
                                    lv_color_hex(Theme::Get().colors().text),
                                    LV_PART_MAIN);
        if (reply_presented_) return;
        reply_presented_ = true;
        if (reply_finish_timer_) { lv_timer_delete(reply_finish_timer_); reply_finish_timer_ = nullptr; }
        lv_anim_delete(agent_label_, SetObjectTranslateX);
        lv_obj_set_style_translate_x(agent_label_, 0, LV_PART_MAIN);
        std::string single_line = reply_caption_;
        std::replace(single_line.begin(), single_line.end(), '\n', ' ');
        std::replace(single_line.begin(), single_line.end(), '\r', ' ');
        lv_label_set_text(agent_label_, single_line.c_str());
        lv_point_t measured{};
        lv_text_get_size(&measured, single_line.c_str(), fonts::MediumBold(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int normal_width = home_active_ ? 360 : 220;
        reply_wide_ = measured.x > normal_width;
        const int width = reply_wide_ ? (home_active_ ? 520 : 380) : normal_width;
        lv_obj_set_x(reply_clip_, home_active_ ? 0 : 140);
        lv_obj_set_width(reply_clip_, width);
        lv_obj_set_width(agent_label_, std::max<int>(measured.x + 2, width));
        if (battery_group_) {
            lv_anim_delete(battery_group_, SetAgentClusterTranslateY);
            lv_anim_t battery;
            lv_anim_init(&battery);
            lv_anim_set_var(&battery, battery_group_);
            lv_anim_set_exec_cb(&battery, SetAgentClusterTranslateY);
            lv_anim_set_values(&battery, lv_obj_get_style_translate_y(battery_group_, LV_PART_MAIN),
                reply_wide_ ? -metrics::kStatusBarHeight : 0);
            lv_anim_set_duration(&battery, 420);
            lv_anim_set_path_cb(&battery, lv_anim_path_ease_in_out);
            lv_anim_start(&battery);
        }
        if (!home_active_) AnimateAgentFace(-117, 420);
        const int distance = std::max<int>(0, measured.x - width);
        const uint32_t travel = static_cast<uint32_t>(distance) * 1000 / 42;
        uint32_t duration = 420 + std::max<uint32_t>(2400, static_cast<uint32_t>(measured.x) * 9);
        if (distance > 0) {
            lv_anim_t animation;
            lv_anim_init(&animation);
            lv_anim_set_var(&animation, agent_label_);
            lv_anim_set_exec_cb(&animation, SetObjectTranslateX);
            lv_anim_set_values(&animation, 0, -distance);
            lv_anim_set_delay(&animation, 1420);
            lv_anim_set_duration(&animation, travel);
            lv_anim_set_path_cb(&animation, lv_anim_path_linear);
            lv_anim_start(&animation);
            duration = 1420 + travel + 1400;
        }
        reply_finish_timer_ = lv_timer_create([](lv_timer_t* timer) {
            auto* self = static_cast<StatusBar*>(lv_timer_get_user_data(timer));
            self->reply_finish_timer_ = nullptr;
            lv_timer_delete(timer);
            self->ClearReplyCaption();
            self->UpdateAgentPresentation();
        }, duration, this);
        return;
    }
    const bool show_activation = !last_center_text_.empty() && !lock_screen_mode_;
    // Home shows no mini face, so the state has to be said in words instead.
    const bool show_state =
        !show_activation && home_active_ && !lock_screen_mode_;
    if (show_activation || show_state) {
        const auto& colors = Theme::Get().colors();
        lv_obj_remove_flag(agent_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(agent_label_,
                                    show_state ? LV_TEXT_ALIGN_CENTER
                                               : LV_TEXT_ALIGN_LEFT,
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(
            agent_label_,
            lv_color_hex(show_state && agent_state_ != AgentState::Idle
                             ? colors.accent
                             : colors.text),
            LV_PART_MAIN);
        lv_label_set_text(agent_label_,
                          show_activation ? last_center_text_.c_str()
                                          : AgentStateText(agent_state_));
        lv_obj_set_style_translate_x(agent_label_, 0, LV_PART_MAIN);
        lv_obj_set_x(reply_clip_, home_active_ ? 0 : 140);
        lv_obj_set_width(reply_clip_, home_active_ ? 360 : 220);
        lv_obj_set_width(agent_label_, home_active_ ? 360 : 220);
    } else lv_obj_add_flag(agent_label_, LV_OBJ_FLAG_HIDDEN);
    if (battery_group_) {
        lv_anim_delete(battery_group_, SetAgentClusterTranslateY);
        lv_obj_set_style_translate_y(battery_group_, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(battery_group_, LV_OPA_COVER, LV_PART_MAIN);
    }
    if (lock_screen_mode_ ||
        (home_active_ && !show_activation && !show_state)) {
        lv_obj_add_flag(agent_cluster_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(agent_cluster_, LV_OBJ_FLAG_HIDDEN);
    }
    AnimateAgentFace(0, 420);
}

namespace {

void RefreshStatusBarAsync(void* user_data) {
    auto* self = static_cast<StatusBar*>(user_data);
    if (self != nullptr) self->Refresh(true);
}

}  // namespace

void StatusBar::RefreshAsync() {
    lv_async_call(RefreshStatusBarAsync, this);
}

void StatusBar::SetVisible(bool visible) {
    visible_ = visible;
    if (root_ == nullptr) return;
    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
        Refresh(true);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void StatusBar::SetAgentClusterTranslateY(void* object, int32_t value) {
    auto* target = static_cast<lv_obj_t*>(object);
    if (target != nullptr && lv_obj_is_valid(target)) {
        lv_obj_set_style_translate_y(target, value, LV_PART_MAIN);
    }
}

void StatusBar::OnAgentClusterExitCompleted(lv_anim_t* animation) {
    auto* self = animation != nullptr
                     ? static_cast<StatusBar*>(lv_anim_get_user_data(animation))
                     : nullptr;
    if (self == nullptr || self->home_active_ || self->agent_cluster_ == nullptr ||
        !lv_obj_is_valid(self->agent_cluster_)) {
        return;
    }
    lv_obj_add_flag(self->agent_cluster_, LV_OBJ_FLAG_HIDDEN);
}

void StatusBar::AnimateAgentCluster(int32_t target, uint32_t duration_ms) {
    if (agent_cluster_ == nullptr || !lv_obj_is_valid(agent_cluster_)) return;
    lv_anim_delete(agent_cluster_, SetAgentClusterTranslateY);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, agent_cluster_);
    lv_anim_set_user_data(&animation, this);
    lv_anim_set_exec_cb(&animation, SetAgentClusterTranslateY);
    lv_anim_set_values(
        &animation, lv_obj_get_style_translate_y(agent_cluster_, LV_PART_MAIN),
        target);
    lv_anim_set_duration(&animation, duration_ms);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    if (target < 0) {
        lv_anim_set_completed_cb(&animation, OnAgentClusterExitCompleted);
    }
    lv_anim_start(&animation);
}

void StatusBar::SetHomeActive(bool active) {
    if (home_active_ == active) {
        UpdateAgentPresentation();
        return;
    }
    home_active_ = active;
    reply_presented_ = false;
    if (agent_cluster_ == nullptr || !lv_obj_is_valid(agent_cluster_)) return;
    lv_anim_delete(agent_cluster_, SetAgentClusterTranslateY);
    if (lock_screen_mode_) {
        lv_obj_add_flag(agent_cluster_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // Home keeps the cluster for the state words; only the mini face steps out.
    lv_obj_remove_flag(agent_cluster_, LV_OBJ_FLAG_HIDDEN);
    if (active) {
        lv_obj_set_style_translate_y(agent_cluster_, 0, LV_PART_MAIN);
    } else {
        lv_obj_set_style_translate_y(agent_cluster_, -metrics::kStatusBarHeight,
                                     LV_PART_MAIN);
        AnimateAgentCluster(0, metrics::kTransitionMs);
    }
    UpdateAgentPresentation();
}

void StatusBar::SetLockScreenMode(bool active) {
    lock_screen_mode_ = active;
    if (time_label_ != nullptr) {
        if (active) {
            lv_obj_add_flag(time_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(time_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (agent_cluster_ != nullptr) {
        if (active) {
            lv_obj_add_flag(agent_cluster_, LV_OBJ_FLAG_HIDDEN);
        } else if (home_active_) {
            lv_obj_add_flag(agent_cluster_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(agent_cluster_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (agent_face_slot_ != nullptr) {
        if (active || !ai_available_) lv_obj_add_flag(agent_face_slot_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(agent_face_slot_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateAgentPresentation();
    if (active) SetVisible(true);
}

void StatusBar::TimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<StatusBar*>(lv_timer_get_user_data(timer));
    if (self != nullptr && self->visible_) self->Refresh(false);
}

void StatusBar::DeletedCallback(lv_event_t* event) {
    auto* self = static_cast<StatusBar*>(lv_event_get_user_data(event));
    if (self == nullptr) return;
    self->ClearReplyCaption();
    if (self->timer_ != nullptr) {
        lv_timer_delete(self->timer_);
        self->timer_ = nullptr;
    }
    if (self->agent_cluster_ != nullptr) {
        lv_anim_delete(self->agent_cluster_, SetAgentClusterTranslateY);
    }
    self->agent_expression_.reset();
    self->root_ = nullptr;
    self->left_cluster_ = nullptr;
    self->time_label_ = nullptr;
    self->agent_cluster_ = nullptr;
    self->agent_face_slot_ = nullptr;
    self->agent_dot_ = nullptr;
    self->agent_label_ = nullptr;
    self->reply_clip_ = nullptr;
    self->right_cluster_ = nullptr;
    self->bluetooth_icon_ = nullptr;
    self->network_icon_ = nullptr;
    self->battery_group_ = nullptr;
    self->battery_icon_ = nullptr;
    self->battery_outline_ = nullptr;
    self->battery_cells_.fill(nullptr);
    self->battery_tip_ = nullptr;
    self->battery_bolt_outline_ = nullptr;
    self->battery_bolt_fill_ = nullptr;
    self->battery_label_ = nullptr;
    self->last_bluetooth_enabled_ = false;
    self->last_bluetooth_connected_ = false;
    self->home_active_ = true;
    self->ai_available_ = true;
    self->reply_caption_.clear();
    self->reply_wide_ = false;
}

}  // namespace agent_ui
