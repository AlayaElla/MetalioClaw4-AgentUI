#include "home_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <esp_timer.h>
#include <font_awesome.h>
#include <wifi_station.h>

#include "application.h"
#include "components/expression_player.h"
#include "components/haptic_feedback.h"
#include "components/ui_components.h"
#include "external_app_manager.h"
#include "core/fonts.h"
#include "core/idle_power.h"
#include "core/navigation.h"
#include "core/theme.h"
#include "dual_network_board.h"
#include "sc7a20_motion.h"

namespace agent_ui::home {
namespace {

constexpr int kHeroTop = metrics::kStatusBarHeight;
constexpr int kHeroHeight = 470;
constexpr int kMessageWidth = 540;
constexpr int kMessageLineSpace = 10;
constexpr int kMessageFirstScrollDelayMs = 1600;
constexpr int kMessageScrollPeriodMs = 1000;
constexpr int kCarouselHeight = 208;
constexpr int kCarouselTop = metrics::kDisplaySize - kCarouselHeight;
constexpr int kCarouselStep = 180;
constexpr int kCarouselItemWidth = 168;
constexpr int kCarouselFocusX =
    (metrics::kDisplaySize - kCarouselItemWidth) / 2;
constexpr int kCarouselNameInset = 8;
constexpr int kCarouselNameWidth =
    kCarouselItemWidth - kCarouselNameInset * 2;
constexpr int kCarouselNameY = 78;
constexpr int kCarouselNameCompactArrowY = 135;
constexpr int kCarouselEdgeWidth = 92;
constexpr int kCarouselCenterX = metrics::kDisplaySize / 2;
constexpr int kCarouselDetentSize = 8;
constexpr int kCarouselDetentVisualX = -24;
constexpr float kCarouselDetentVisualY = 42.0f;
constexpr int kDragThreshold = 10;
constexpr int kMotionPeriodMs = 16;
constexpr float kMaxVelocity = 3.1f;
constexpr float kReleaseBoost = 1.3f;
constexpr float kFriction = 0.95f;
constexpr float kStopVelocity = 0.025f;
constexpr float kMinReleaseVelocity = 0.06f;
constexpr float kSnapDurationMs = 240.0f;
constexpr uint32_t kConversationLayoutDurationMs = 750;
constexpr int kConversationMessageOffset = 466;
constexpr int kConversationRuleOffset = 281;
constexpr int kConversationCarouselOffset = kCarouselHeight + 24;
constexpr uint32_t kStandbyChromeDurationMs = 260;
constexpr int kStandbyTopOffset = -kHeroHeight;
constexpr int kStandbyBottomOffset = 230;
constexpr int kParallaxPeriodMs = 33;
constexpr float kParallaxResponseMs = 130.0f;
constexpr float kCarouselParallaxPixels = 8.0f;
constexpr float kMessageParallaxPixels = 18.0f;
constexpr float kExpressionParallaxPixels = 32.0f;
static_assert(kCarouselParallaxPixels < kMessageParallaxPixels &&
                  kMessageParallaxPixels < kExpressionParallaxPixels,
              "Home parallax layers must increase from carousel to expression");

int MessageViewportHeight() {
    return fonts::LargeBold()->line_height * 2 + kMessageLineSpace;
}

bool IsAsciiAlphaNumeric(char value) {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

bool IsEnglishPunctuation(const std::string& text, size_t index) {
    const bool follows_word =
        index > 0 && IsAsciiAlphaNumeric(text[index - 1]);
    const bool precedes_word =
        index + 1 < text.size() && IsAsciiAlphaNumeric(text[index + 1]);
    return follows_word || precedes_word;
}

size_t TrailingPunctuationSize(const std::string& text, size_t index) {
    if (index >= text.size()) return 0;
    switch (text[index]) {
        case ',':
        case '.':
        case ';':
        case '!':
        case '?':
        case ':':
        case '~':
        case '"':
        case '\'':
        case ')':
        case ']':
        case '}':
            return 1;
        default:
            break;
    }

    static constexpr std::array<std::string_view, 22> kPunctuation = {
        "，", "。", "；", "！", "？", "：", "、", "…",
        "‥", "～", "〜", "—", "–", "·",
        "”", "’", "）", "》", "】", "」", "』", "〉",
    };
    for (const std::string_view punctuation : kPunctuation) {
        if (text.compare(index, punctuation.size(), punctuation.data(),
                         punctuation.size()) == 0) {
            return punctuation.size();
        }
    }
    return 0;
}

size_t SentenceBreakPunctuationSize(const std::string& text, size_t index) {
    if (index >= text.size()) return 0;
    if ((text[index] == ',' || text[index] == '.' || text[index] == ';') &&
        !IsEnglishPunctuation(text, index)) {
        return 1;
    }
    if (text.compare(index, 3, "，") == 0 ||
        text.compare(index, 3, "。") == 0 ||
        text.compare(index, 3, "；") == 0) {
        return 3;
    }
    return 0;
}

size_t Utf8CharacterSize(const std::string& text, size_t index) {
    if (index >= text.size()) return 0;
    const uint8_t lead = static_cast<uint8_t>(text[index]);
    if ((lead & 0x80u) == 0) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;
}

bool StartsWithSingleTextPunctuationGroup(const std::string& text,
                                          size_t index) {
    if (index >= text.size() || text[index] == '\n' || text[index] == '\r' ||
        TrailingPunctuationSize(text, index) != 0) {
        return false;
    }

    const size_t character_size = Utf8CharacterSize(text, index);
    if (character_size <= 1 || index + character_size >= text.size()) {
        return false;
    }
    return TrailingPunctuationSize(text, index + character_size) != 0;
}

std::string FormatConversationText(const char* text) {
    std::string result;
    const std::string source = text != nullptr ? text : "";
    result.reserve(source.size() + 8);

    for (size_t index = 0; index < source.size();) {
        const size_t punctuation_size =
            SentenceBreakPunctuationSize(source, index);

        if (punctuation_size == 0) {
            result.push_back(source[index++]);
            continue;
        }

        result.append(source, index, punctuation_size);
        index += punctuation_size;
        while (index < source.size()) {
            const size_t trailing_size =
                TrailingPunctuationSize(source, index);
            if (trailing_size == 0) break;
            result.append(source, index, trailing_size);
            index += trailing_size;
        }
        if (StartsWithSingleTextPunctuationGroup(source, index)) {
            continue;
        }
        if (index < source.size() && source[index] != '\n' &&
            source[index] != '\r') {
            result.push_back('\n');
        }
    }
    return result;
}

struct AppDefinition {
    ScreenId id;
    std::string number;
    std::string name;
    std::string external_id;
    bool requires_network;
};

const std::array<AppDefinition, 5> kBuiltInApps = {{
    {ScreenId::Codex, "01", "Codex", "", true},
    {ScreenId::Phone, "02", "电话", "", false},
    {ScreenId::Files, "03", "文件", "", false},
    {ScreenId::Camera, "04", "相机", "", false},
    {ScreenId::Settings, "05", "设置", "", false},
}};

std::vector<AppDefinition> BuildAppDefinitions() {
    std::vector<AppDefinition> definitions(kBuiltInApps.begin(), kBuiltInApps.end());
    const auto& external = external_apps::Manager::Get().apps();
    definitions.reserve(definitions.size() + external.size());
    for (const external_apps::AppInfo& app : external) {
        std::string number = std::to_string(definitions.size() + 1);
        if (number.size() < 2) number.insert(number.begin(), '0');
        definitions.push_back(
            {ScreenId::ExternalAppHost, number, app.name, app.id, false});
    }
    return definitions;
}

struct ArcItem {
    lv_obj_t* button = nullptr;
    lv_obj_t* rule = nullptr;
    lv_obj_t* number = nullptr;
    lv_obj_t* name = nullptr;
    lv_obj_t* arrow = nullptr;
};

struct ParallaxLayer {
    lv_obj_t* object = nullptr;
    int last_x = 0;
    int last_y = 0;
};

struct HomeState {
    lv_obj_t* root = nullptr;
    lv_obj_t* message_viewport = nullptr;
    lv_obj_t* message = nullptr;
    lv_obj_t* editorial_rule = nullptr;
    lv_obj_t* listening_hit_area = nullptr;
    lv_obj_t* conversation_exit_hit_area = nullptr;
    lv_obj_t* network_guard_overlay = nullptr;
    lv_obj_t* standby_time = nullptr;
    lv_obj_t* standby_date = nullptr;
    lv_obj_t* standby_rule = nullptr;
    lv_obj_t* carousel = nullptr;
    lv_obj_t* carousel_cache = nullptr;
    lv_obj_t* left_fade = nullptr;
    lv_obj_t* right_fade = nullptr;
    lv_draw_buf_t* carousel_snapshot = nullptr;
    lv_draw_buf_t* left_fade_mask = nullptr;
    lv_draw_buf_t* right_fade_mask = nullptr;
    std::vector<AppDefinition> app_definitions;
    std::vector<ArcItem> apps;
    std::vector<lv_obj_t*> half_detents;
    ExpressionPlayer* expression = nullptr;
    lv_timer_t* motion_timer = nullptr;
    lv_timer_t* message_timer = nullptr;
    lv_timer_t* conversation_layout_timer = nullptr;
    lv_timer_t* standby_clock_timer = nullptr;
    lv_timer_t* standby_hide_timer = nullptr;
    lv_timer_t* parallax_timer = nullptr;
    ParallaxLayer expression_parallax{};
    ParallaxLayer message_parallax{};
    ParallaxLayer editorial_parallax{};
    std::array<ParallaxLayer, 4> carousel_parallax{};
    float parallax_x = 0.0f;
    float parallax_y = 0.0f;
    int64_t parallax_last_us = 0;
    RendererActions actions;
    std::string rendered_message;
    int focused_index = 0;
    float carousel_offset = 0.0f;
    float carousel_position = 0.0f;
    float carousel_velocity = 0.0f;
    float snap_start_offset = 0.0f;
    float snap_target_position = 0.0f;
    float snap_elapsed_ms = 0.0f;
    int pointer_start_x = 0;
    int pointer_last_x = 0;
    int64_t pointer_last_us = 0;
    int64_t motion_last_us = 0;
    bool pointer_active = false;
    bool pointer_moved = false;
    bool suppress_click = false;
    bool snapping = false;
    bool battery_initialized = false;
    bool charging = false;
    bool charging_candidate = false;
    uint32_t charging_candidate_since_tick = 0;
    bool conversation_active = false;
    bool standby = false;
    ScreenId pending_screen = ScreenId::Home;
};

HomeState* s_state = nullptr;
int s_saved_focused_index = 0;

void DeleteMotionTimer(HomeState* state);
void DeleteParallaxTimer(HomeState* state);

ParallaxLayer* FindParallaxLayer(lv_obj_t* object) {
    if (s_state == nullptr || object == nullptr) return nullptr;
    if (s_state->expression_parallax.object == object) {
        return &s_state->expression_parallax;
    }
    if (s_state->message_parallax.object == object) {
        return &s_state->message_parallax;
    }
    if (s_state->editorial_parallax.object == object) {
        return &s_state->editorial_parallax;
    }
    for (ParallaxLayer& layer : s_state->carousel_parallax) {
        if (layer.object == object) return &layer;
    }
    return nullptr;
}

bool RequiresNetwork(ScreenId screen) {
    for (const auto& app : kBuiltInApps) {
        if (app.id == screen) return app.requires_network;
    }
    return false;
}

bool ShouldBlockForDisconnectedWifi(ScreenId screen) {
    if (!RequiresNetwork(screen)) return false;
    const NetworkType type =
        DualNetworkBoard::LoadNetworkTypeFromSettings(1);
    return type == NetworkType::WIFI &&
           !WifiStation::GetInstance().IsConnected();
}

void DismissNetworkGuard(lv_event_t* event) {
    auto* state = static_cast<HomeState*>(lv_event_get_user_data(event));
    if (state == nullptr || state->network_guard_overlay == nullptr) return;
    lv_obj_delete(state->network_guard_overlay);
    state->network_guard_overlay = nullptr;
}

void ShowNetworkGuard(HomeState* state) {
    if (state == nullptr || state->root == nullptr ||
        state->network_guard_overlay != nullptr) {
        return;
    }
    const auto& colors = Theme::Get().colors();
    lv_obj_t* overlay = ui_components::CreateModalOverlay(state->root);
    lv_obj_t* card = ui_components::CreateModalSurface(overlay, 560, 286);
    if (overlay == nullptr || card == nullptr) return;
    state->network_guard_overlay = overlay;
    lv_obj_center(card);

    lv_obj_t* icon = lv_label_create(card);
    lv_label_set_text(icon, FONT_AWESOME_WIFI_SLASH);
    lv_obj_set_style_text_font(icon, fonts::IconLarge(), LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(colors.warning), LV_PART_MAIN);
    lv_obj_set_pos(icon, 28, 34);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "Wi-Fi 未连接");
    lv_obj_set_style_text_font(title, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(title, 108, 34);

    lv_obj_t* detail = lv_label_create(card);
    lv_label_set_text(detail, "此应用需要网络连接，请先在设置中连接 Wi-Fi。");
    lv_obj_set_size(detail, 424, 84);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(detail, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(detail, lv_color_hex(colors.muted), LV_PART_MAIN);
    lv_obj_set_pos(detail, 108, 84);

    lv_obj_t* close = ui_components::CreateButton(card);
    lv_obj_set_size(close, 160, 58);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -28, -24);
    lv_obj_set_style_bg_color(close, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(close, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(close, DismissNetworkGuard, LV_EVENT_CLICKED, state);
    lv_obj_t* close_label = lv_label_create(close);
    lv_label_set_text(close_label, "知道了");
    lv_obj_set_style_text_font(close_label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(close_label, lv_color_hex(colors.accent_ink),
                                LV_PART_MAIN);
    lv_obj_center(close_label);
}

void SetObjectTranslateY(void* object, int32_t value) {
    auto* target = static_cast<lv_obj_t*>(object);
    if (target != nullptr && lv_obj_is_valid(target)) {
        const ParallaxLayer* layer = FindParallaxLayer(target);
        lv_obj_set_style_translate_y(
            target, value + (layer != nullptr ? layer->last_y : 0),
            LV_PART_MAIN);
    }
}

void AnimateTranslateY(lv_obj_t* object, int32_t target,
                       uint32_t duration_ms = kStandbyChromeDurationMs) {
    if (object == nullptr || !lv_obj_is_valid(object)) return;
    lv_anim_delete(object, SetObjectTranslateY);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, SetObjectTranslateY);
    const ParallaxLayer* layer = FindParallaxLayer(object);
    const int32_t current_base =
        lv_obj_get_style_translate_y(object, LV_PART_MAIN) -
        (layer != nullptr ? layer->last_y : 0);
    lv_anim_set_values(
        &animation, current_base, target);
    lv_anim_set_duration(&animation, duration_ms);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_start(&animation);
}

void DeleteConversationLayoutTimer(HomeState* state) {
    if (state == nullptr || state->conversation_layout_timer == nullptr) return;
    lv_timer_delete(state->conversation_layout_timer);
    state->conversation_layout_timer = nullptr;
}

void SetCarouselInteractive(HomeState* state, bool interactive) {
    if (state == nullptr || state->carousel == nullptr) return;
    if (interactive) {
        lv_obj_add_flag(state->carousel, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(state->carousel, LV_OBJ_FLAG_CLICKABLE);
        state->pointer_active = false;
        state->pointer_moved = false;
        state->suppress_click = false;
        state->carousel_velocity = 0.0f;
        state->snapping = false;
    }
    for (ArcItem& item : state->apps) {
        if (item.button == nullptr) continue;
        if (interactive) {
            lv_obj_add_flag(item.button, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_remove_flag(item.button, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

void SetConversationExitHitArea(HomeState* state, bool active) {
    if (state == nullptr || state->conversation_exit_hit_area == nullptr) return;
    if (active) {
        lv_obj_remove_flag(state->conversation_exit_hit_area,
                           LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(state->conversation_exit_hit_area);
    } else {
        lv_obj_add_flag(state->conversation_exit_hit_area,
                        LV_OBJ_FLAG_HIDDEN);
    }
}

void SetCarouselRenderingHidden(HomeState* state, bool hidden) {
    if (state == nullptr) return;
    const std::array<lv_obj_t*, 4> objects = {
        state->carousel, state->carousel_cache, state->left_fade,
        state->right_fade,
    };
    for (lv_obj_t* object : objects) {
        if (object == nullptr) continue;
        if (hidden) {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!hidden) {
        if (state->carousel_snapshot != nullptr &&
            state->carousel_cache != nullptr) {
            lv_obj_remove_flag(state->carousel_cache, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(state->carousel, LV_OPA_TRANSP, LV_PART_MAIN);
        } else if (state->carousel_cache != nullptr) {
            lv_obj_add_flag(state->carousel_cache, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(state->carousel, LV_OPA_COVER, LV_PART_MAIN);
        }
    }
}

void FinishConversationLayout(lv_timer_t* timer) {
    auto* state = static_cast<HomeState*>(lv_timer_get_user_data(timer));
    if (state == nullptr || state->conversation_layout_timer != timer) return;
    state->conversation_layout_timer = nullptr;
    if (state->standby) return;
    if (state->conversation_active) {
        SetCarouselRenderingHidden(state, true);
    } else {
        SetCarouselInteractive(state, true);
    }
}

void ScheduleConversationLayoutFinish(HomeState* state,
                                      uint32_t duration_ms) {
    if (state == nullptr) return;
    DeleteConversationLayoutTimer(state);
    state->conversation_layout_timer =
        lv_timer_create(FinishConversationLayout, duration_ms, state);
    lv_timer_set_repeat_count(state->conversation_layout_timer, 1);
}

void AnimateConversationLayout(HomeState* state, bool active) {
    if (state == nullptr || state->conversation_active == active) return;
    state->conversation_active = active;
    if (state->standby) return;

    DeleteMotionTimer(state);
    SetConversationExitHitArea(state, active);
    SetCarouselRenderingHidden(state, false);
    SetCarouselInteractive(state, false);
    AnimateTranslateY(state->message_viewport,
                      active ? kConversationMessageOffset : 0,
                      kConversationLayoutDurationMs);
    AnimateTranslateY(state->editorial_rule,
                      active ? kConversationRuleOffset : 0,
                      kConversationLayoutDurationMs);
    const int carousel_offset = active ? kConversationCarouselOffset : 0;
    AnimateTranslateY(state->carousel, carousel_offset,
                      kConversationLayoutDurationMs);
    AnimateTranslateY(state->carousel_cache, carousel_offset,
                      kConversationLayoutDurationMs);
    AnimateTranslateY(state->left_fade, carousel_offset,
                      kConversationLayoutDurationMs);
    AnimateTranslateY(state->right_fade, carousel_offset,
                      kConversationLayoutDurationMs);
    ScheduleConversationLayoutFinish(state, kConversationLayoutDurationMs);
}

void AnimateHomeChrome(HomeState* state, bool visible) {
    if (state == nullptr) return;
    AnimateTranslateY(state->message_viewport,
                      visible ? 0 : kStandbyTopOffset);
    AnimateTranslateY(state->editorial_rule,
                      visible ? 0 : kStandbyTopOffset);
    AnimateTranslateY(state->carousel,
                      visible ? 0 : kStandbyBottomOffset);
    AnimateTranslateY(state->carousel_cache,
                      visible ? 0 : kStandbyBottomOffset);
    AnimateTranslateY(state->left_fade,
                      visible ? 0 : kStandbyBottomOffset);
    AnimateTranslateY(state->right_fade,
                      visible ? 0 : kStandbyBottomOffset);
}

void UpdateStandbyClock(HomeState* state) {
    if (state == nullptr || state->standby_time == nullptr ||
        state->standby_date == nullptr) {
        return;
    }
    const time_t now = std::time(nullptr);
    struct tm local = {};
    if (localtime_r(&now, &local) == nullptr || local.tm_year < 125) {
        lv_label_set_text(state->standby_time, "00:00");
        lv_label_set_text(state->standby_date, "--月--日");
        return;
    }
    char time_text[16];
    std::strftime(time_text, sizeof(time_text), "%H:%M", &local);
    lv_label_set_text(state->standby_time, time_text);
    static constexpr const char* kWeekdays[] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
    };
    char date_text[32];
    std::snprintf(date_text, sizeof(date_text), "%d月%d日 %s", local.tm_mon + 1,
                  local.tm_mday, kWeekdays[local.tm_wday]);
    lv_label_set_text(state->standby_date, date_text);
}

void OnStandbyClock(lv_timer_t* timer) {
    UpdateStandbyClock(
        static_cast<HomeState*>(lv_timer_get_user_data(timer)));
}

void DeleteStandbyClockTimer(HomeState* state) {
    if (state == nullptr || state->standby_clock_timer == nullptr) return;
    lv_timer_delete(state->standby_clock_timer);
    state->standby_clock_timer = nullptr;
}

void StartStandbyClockTimer(HomeState* state) {
    if (state == nullptr || state->standby_clock_timer != nullptr ||
        !state->standby) {
        return;
    }
    UpdateStandbyClock(state);
    state->standby_clock_timer =
        lv_timer_create(OnStandbyClock, 1000, state);
}

void HideStandbyChrome(lv_timer_t* timer) {
    auto* state = static_cast<HomeState*>(lv_timer_get_user_data(timer));
    if (state == nullptr || state->standby_hide_timer != timer) return;
    state->standby_hide_timer = nullptr;
    if (state->standby_time != nullptr) {
        lv_obj_add_flag(state->standby_time, LV_OBJ_FLAG_HIDDEN);
    }
    if (state->standby_date != nullptr) {
        lv_obj_add_flag(state->standby_date, LV_OBJ_FLAG_HIDDEN);
    }
    if (state->standby_rule != nullptr) {
        lv_obj_add_flag(state->standby_rule, LV_OBJ_FLAG_HIDDEN);
    }
}

void AnimateStandbyChrome(HomeState* state, bool visible) {
    if (state == nullptr) return;
    if (state->standby_hide_timer != nullptr) {
        lv_timer_delete(state->standby_hide_timer);
        state->standby_hide_timer = nullptr;
    }
    if (visible) {
        lv_obj_remove_flag(state->standby_time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(state->standby_date, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(state->standby_rule, LV_OBJ_FLAG_HIDDEN);
        StartStandbyClockTimer(state);
    } else {
        DeleteStandbyClockTimer(state);
        state->standby_hide_timer = lv_timer_create(
            HideStandbyChrome, kStandbyChromeDurationMs, state);
        lv_timer_set_repeat_count(state->standby_hide_timer, 1);
    }
    const int target = visible ? 0 : kStandbyBottomOffset;
    AnimateTranslateY(state->standby_time, target);
    AnimateTranslateY(state->standby_date, target);
    AnimateTranslateY(state->standby_rule, target);
}

uint8_t FadeOpacity(uint8_t position) {
    constexpr int kMiddleStop = 133;
    constexpr int kEdgeOpacity = 209;    // Demo: 82%
    constexpr int kMiddleOpacity = 138;  // Demo: 54%
    if (position <= kMiddleStop) {
        return static_cast<uint8_t>(
            kEdgeOpacity - (kEdgeOpacity - kMiddleOpacity) * position /
                               kMiddleStop);
    }
    return static_cast<uint8_t>(
        kMiddleOpacity * (255 - position) / (255 - kMiddleStop));
}

lv_obj_t* CreateFadeMask(HomeState* state, bool left) {
    const auto& colors = Theme::Get().colors();
    lv_draw_buf_t*& buffer = left ? state->left_fade_mask
                                  : state->right_fade_mask;
    buffer = lv_draw_buf_create(kCarouselEdgeWidth, kCarouselHeight,
                                LV_COLOR_FORMAT_A8, 0);

    lv_obj_t* mask = nullptr;
    if (buffer != nullptr && buffer->data != nullptr) {
        auto* pixels = static_cast<uint8_t*>(buffer->data);
        for (int y = 0; y < kCarouselHeight; ++y) {
            for (int x = 0; x < kCarouselEdgeWidth; ++x) {
                const int distance = left ? x : kCarouselEdgeWidth - 1 - x;
                const auto position = static_cast<uint8_t>(
                    distance * 255 / (kCarouselEdgeWidth - 1));
                pixels[y * buffer->header.stride + x] = FadeOpacity(position);
            }
        }
        mask = lv_image_create(state->root);
        lv_image_set_src(mask, buffer);
        lv_obj_set_style_image_recolor(
            mask, lv_color_hex(colors.background), LV_PART_MAIN);
        lv_obj_set_style_image_recolor_opa(mask, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        // Allocation failure still keeps a translucent edge instead of
        // reverting to the visually harsh opaque block.
        if (buffer != nullptr) {
            lv_draw_buf_destroy(buffer);
            buffer = nullptr;
        }
        mask = lv_obj_create(state->root);
        lv_obj_remove_style_all(mask);
        lv_obj_set_style_bg_color(mask, lv_color_hex(colors.background),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(mask, LV_OPA_50, LV_PART_MAIN);
    }
    lv_obj_set_size(mask, kCarouselEdgeWidth, kCarouselHeight);
    lv_obj_set_pos(mask, left ? 0 : metrics::kDisplaySize - kCarouselEdgeWidth,
                   kCarouselTop);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    return mask;
}

void ShowLiveCarousel(HomeState* state) {
    if (state == nullptr || state->carousel == nullptr) return;
    lv_obj_set_style_opa(state->carousel, LV_OPA_COVER, LV_PART_MAIN);
    if (state->carousel_cache != nullptr) {
        lv_obj_add_flag(state->carousel_cache, LV_OBJ_FLAG_HIDDEN);
    }
}

void CacheSettledCarousel(HomeState* state) {
    if (state == nullptr || state->carousel == nullptr ||
        state->carousel_cache == nullptr) {
        return;
    }
#if CONFIG_LV_USE_SNAPSHOT
    ShowLiveCarousel(state);
    lv_obj_update_layout(state->carousel);
    lv_draw_buf_t* snapshot =
        lv_snapshot_take(state->carousel, LV_COLOR_FORMAT_RGB565);
    if (snapshot == nullptr) return;

    lv_image_set_src(state->carousel_cache, nullptr);
    if (state->carousel_snapshot != nullptr) {
        lv_image_cache_drop(state->carousel_snapshot);
        lv_draw_buf_destroy(state->carousel_snapshot);
    }
    state->carousel_snapshot = snapshot;
    lv_image_set_src(state->carousel_cache, state->carousel_snapshot);
    lv_obj_remove_flag(state->carousel_cache, LV_OBJ_FLAG_HIDDEN);
    // Keep the real carousel interactive, but skip its children in the idle
    // draw path. It is restored as soon as a new gesture begins.
    lv_obj_set_style_opa(state->carousel, LV_OPA_TRANSP, LV_PART_MAIN);
#endif
}

int WrapIndex(const HomeState* state, int index) {
    const int count = state == nullptr ? 0 : static_cast<int>(state->apps.size());
    if (count == 0) return 0;
    index %= count;
    return index < 0 ? index + count : index;
}

int CircularDistance(const HomeState* state, int index, int focused_index) {
    const int count = state == nullptr ? 0 : static_cast<int>(state->apps.size());
    if (count == 0) return 0;
    int distance = WrapIndex(state, index - focused_index);
    if (distance > count / 2) distance -= count;
    return distance;
}

void ApplyFocus(HomeState* state, int focused_index) {
    if (state == nullptr) return;
    state->focused_index = WrapIndex(state, focused_index);
    const auto& colors = Theme::Get().colors();
    for (size_t index = 0; index < state->apps.size(); ++index) {
        ArcItem& item = state->apps[index];
        const bool focused = static_cast<int>(index) == state->focused_index;
        const uint32_t primary = focused ? colors.accent : colors.text;
        lv_obj_set_style_bg_color(item.rule, lv_color_hex(primary), LV_PART_MAIN);
        lv_obj_set_style_text_color(item.number, lv_color_hex(primary), LV_PART_MAIN);
        lv_obj_set_style_text_color(item.name, lv_color_hex(colors.text), LV_PART_MAIN);
        lv_obj_set_style_text_color(item.arrow, lv_color_hex(primary), LV_PART_MAIN);
    }
}

void ApplyCarouselGeometry(HomeState* state) {
    if (state == nullptr || state->carousel == nullptr) return;
    for (size_t index = 0; index < state->apps.size(); ++index) {
        ArcItem& item = state->apps[index];
        const int order = CircularDistance(state, static_cast<int>(index),
                                           state->focused_index);
        const int x = kCarouselFocusX + order * kCarouselStep +
                      static_cast<int>(std::lround(state->carousel_offset));
        const int item_center = x + kCarouselItemWidth / 2;
        const float distance = std::min(
            1.0f, std::abs(static_cast<float>(item_center - kCarouselCenterX)) /
                      (kCarouselStep * 2.0f));
        const int curve_y = static_cast<int>(std::lround(
            49.0f - 42.0f * std::pow(distance, 1.55f)));
        lv_obj_set_pos(item.button, x, curve_y);
    }
    for (size_t index = 0; index < state->half_detents.size(); ++index) {
        const int leading_order = CircularDistance(
            state, static_cast<int>(index), state->focused_index);
        const int leading_center_x =
            kCarouselCenterX + leading_order * kCarouselStep +
            static_cast<int>(std::lround(state->carousel_offset));
        const int trailing_center_x = leading_center_x + kCarouselStep;
        const int center_x = (leading_center_x + trailing_center_x) / 2;
        const float distance = std::min(
            1.0f, std::abs(static_cast<float>(center_x - kCarouselCenterX)) /
                      (kCarouselStep * 2.0f));
        const int curve_y = static_cast<int>(std::lround(
            49.0f - 42.0f * std::pow(distance, 1.55f)));
        lv_obj_set_pos(
            state->half_detents[index],
            center_x - kCarouselDetentSize / 2 + kCarouselDetentVisualX,
            static_cast<int>(std::lround(
                curve_y - 2.5f + kCarouselDetentVisualY)));
    }
}

void PlayCarouselTick(HomeState* state) {
    if (state != nullptr && state->actions.play_carousel_tick) {
        state->actions.play_carousel_tick();
    }
}

void PlayCarouselDetents(HomeState* state, float previous_position,
                         float next_position) {
    constexpr float kEpsilon = 0.000001f;
    if (next_position > previous_position) {
        const int first = static_cast<int>(
                              std::floor(previous_position * 2.0f + kEpsilon)) +
                          1;
        const int last = static_cast<int>(
            std::floor(next_position * 2.0f + kEpsilon));
        for (int marker = first; marker <= last; ++marker) {
            PlayCarouselTick(state);
            PlayHaptic(HapticStrength::Light);
        }
    } else if (next_position < previous_position) {
        const int first = static_cast<int>(
                              std::ceil(previous_position * 2.0f - kEpsilon)) -
                          1;
        const int last = static_cast<int>(
            std::ceil(next_position * 2.0f - kEpsilon));
        for (int marker = first; marker >= last; --marker) {
            PlayCarouselTick(state);
            PlayHaptic(HapticStrength::Light);
        }
    }
}

void AdvanceCarousel(HomeState* state, float delta) {
    if (state == nullptr) return;
    const float previous_position = state->carousel_position;
    state->carousel_position -= delta / kCarouselStep;
    state->carousel_offset += delta;
    while (state->carousel_offset <= -kCarouselStep / 2.0f) {
        state->carousel_offset += kCarouselStep;
        ApplyFocus(state, state->focused_index + 1);
    }
    while (state->carousel_offset >= kCarouselStep / 2.0f) {
        state->carousel_offset -= kCarouselStep;
        ApplyFocus(state, state->focused_index - 1);
    }
    PlayCarouselDetents(state, previous_position, state->carousel_position);
    ApplyCarouselGeometry(state);
}

void DeleteMotionTimer(HomeState* state) {
    if (state == nullptr || state->motion_timer == nullptr) return;
    lv_timer_t* timer = state->motion_timer;
    state->motion_timer = nullptr;
    lv_timer_delete(timer);
}

void ApplyParallaxLayer(ParallaxLayer* layer, int target_x, int target_y,
                        bool include_hidden) {
    if (layer == nullptr || layer->object == nullptr ||
        !lv_obj_is_valid(layer->object)) {
        return;
    }
    if (!include_hidden &&
        lv_obj_has_flag(layer->object, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    // The style translation is also used by the Home conversation/standby
    // animations. Remove only the offset written by the previous parallax
    // frame, then add the new one, so every frame stays relative to its own
    // animated baseline instead of accumulating pixels.
    const int32_t base_x = lv_obj_get_style_translate_x(layer->object,
                                                         LV_PART_MAIN) -
                           layer->last_x;
    const int32_t base_y = lv_obj_get_style_translate_y(layer->object,
                                                         LV_PART_MAIN) -
                           layer->last_y;
    if (base_x + target_x !=
            lv_obj_get_style_translate_x(layer->object, LV_PART_MAIN) ||
        base_y + target_y !=
            lv_obj_get_style_translate_y(layer->object, LV_PART_MAIN)) {
        lv_obj_set_style_translate_x(layer->object, base_x + target_x,
                                      LV_PART_MAIN);
        lv_obj_set_style_translate_y(layer->object, base_y + target_y,
                                      LV_PART_MAIN);
    }
    layer->last_x = target_x;
    layer->last_y = target_y;
}

void ApplyParallaxOffsets(HomeState* state, float x, float y,
                          bool include_hidden) {
    if (state == nullptr) return;
    ApplyParallaxLayer(
        &state->expression_parallax,
        static_cast<int>(std::lround(x * kExpressionParallaxPixels)),
        static_cast<int>(std::lround(y * kExpressionParallaxPixels)),
        include_hidden);
    const int message_x =
        static_cast<int>(std::lround(x * kMessageParallaxPixels));
    const int message_y =
        static_cast<int>(std::lround(y * kMessageParallaxPixels));
    ApplyParallaxLayer(&state->message_parallax, message_x, message_y,
                       include_hidden);
    ApplyParallaxLayer(&state->editorial_parallax, message_x, message_y,
                       include_hidden);
    const int carousel_x =
        static_cast<int>(std::lround(x * kCarouselParallaxPixels));
    const int carousel_y =
        static_cast<int>(std::lround(y * kCarouselParallaxPixels));
    for (ParallaxLayer& layer : state->carousel_parallax) {
        ApplyParallaxLayer(&layer, carousel_x, carousel_y, include_hidden);
    }
}

void ResetParallax(HomeState* state) {
    if (state == nullptr) return;
    state->parallax_x = 0.0f;
    state->parallax_y = 0.0f;
    ApplyParallaxOffsets(state, 0.0f, 0.0f, true);
}

float ApproachParallax(float current, float target, float elapsed_ms) {
    const float alpha =
        1.0f - std::exp(-std::clamp(elapsed_ms, 1.0f, 100.0f) /
                        kParallaxResponseMs);
    return current + (target - current) * std::clamp(alpha, 0.0f, 1.0f);
}

void OnParallaxTimer(lv_timer_t* timer) {
    auto* state = static_cast<HomeState*>(lv_timer_get_user_data(timer));
    if (state == nullptr || state->parallax_timer != timer ||
        s_state != state) {
        return;
    }
    // Navigation updates current_ before the outgoing screen is destroyed.
    // Stop here so an invisible Home screen never queues LVGL work during the
    // transition to another app.
    if (Navigation::Get().current() != ScreenId::Home || state->standby) {
        DeleteParallaxTimer(state);
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    float elapsed_ms = static_cast<float>(now_us - state->parallax_last_us) /
                       1000.0f;
    state->parallax_last_us = now_us;
    elapsed_ms = std::clamp(elapsed_ms, 1.0f, 100.0f);

    Sc7a20Tilt tilt;
    const bool has_tilt =
        Sc7a20MotionService::GetInstance().ReadTilt(&tilt) && tilt.valid;
    const float target_x = has_tilt
                               ? std::clamp(tilt.x_q10 / 1000.0f, -1.0f, 1.0f)
                               : 0.0f;
    const float target_y = has_tilt
                               ? std::clamp(tilt.y_q10 / 1000.0f, -1.0f, 1.0f)
                               : 0.0f;
    state->parallax_x =
        ApproachParallax(state->parallax_x, target_x, elapsed_ms);
    state->parallax_y =
        ApproachParallax(state->parallax_y, target_y, elapsed_ms);
    ApplyParallaxOffsets(state, state->parallax_x, state->parallax_y, false);
}

void StartParallaxTimer(HomeState* state) {
    if (state == nullptr || state->parallax_timer != nullptr ||
        state->standby) {
        return;
    }
    state->parallax_last_us = esp_timer_get_time();
    state->parallax_timer =
        lv_timer_create(OnParallaxTimer, kParallaxPeriodMs, state);
}

void DeleteParallaxTimer(HomeState* state) {
    if (state == nullptr || state->parallax_timer == nullptr) return;
    lv_timer_t* timer = state->parallax_timer;
    state->parallax_timer = nullptr;
    lv_timer_delete(timer);
}

void DeleteMessageTimer(HomeState* state) {
    if (state == nullptr || state->message_timer == nullptr) return;
    lv_timer_t* timer = state->message_timer;
    state->message_timer = nullptr;
    lv_timer_delete(timer);
}

void OnMessageTimer(lv_timer_t* timer) {
    auto* state = static_cast<HomeState*>(lv_timer_get_user_data(timer));
    if (state == nullptr || state->message_timer != timer ||
        state->message == nullptr) {
        return;
    }

    const int line_advance =
        fonts::LargeBold()->line_height + kMessageLineSpace;
    const int final_y = std::min<int>(
        0, MessageViewportHeight() -
               static_cast<int>(lv_obj_get_height(state->message)));
    const int next_y = std::max<int>(
        final_y, static_cast<int>(lv_obj_get_y(state->message)) - line_advance);
    lv_obj_set_y(state->message, next_y);
    if (next_y <= final_y) {
        DeleteMessageTimer(state);
    } else {
        lv_timer_set_period(timer, kMessageScrollPeriodMs);
    }
}

void StartMessageScroll(HomeState* state) {
    if (state == nullptr || state->message == nullptr) return;
    DeleteMessageTimer(state);
    lv_obj_set_y(state->message, 0);
    if (lv_obj_get_height(state->message) <= MessageViewportHeight()) return;
    state->message_timer = lv_timer_create(
        OnMessageTimer, kMessageFirstScrollDelayMs, state);
}

void FinishCarouselMotion(HomeState* state) {
    if (state == nullptr) return;
    constexpr float kSettledMovementEpsilon = 0.001f;
    const bool settled_after_movement =
        std::abs(state->snap_start_offset) >= kSettledMovementEpsilon;
    const bool reached_app = std::abs(state->snap_start_offset) >= 0.5f;
    DeleteMotionTimer(state);
    state->carousel_velocity = 0.0f;
    state->carousel_offset = 0.0f;
    state->carousel_position = state->snap_target_position;
    state->snap_elapsed_ms = 0.0f;
    state->snapping = false;
    ApplyCarouselGeometry(state);
    if (reached_app) {
        PlayCarouselTick(state);
    }
    if (settled_after_movement) {
        PlayHaptic(HapticStrength::Light);
    }
    CacheSettledCarousel(state);
}

void BeginSnap(HomeState* state) {
    if (state == nullptr) return;
    state->carousel_velocity = 0.0f;
    state->snap_start_offset = state->carousel_offset;
    state->snap_target_position = std::round(state->carousel_position);
    state->snap_elapsed_ms = 0.0f;
    state->snapping = true;
    if (std::abs(state->snap_start_offset) < 0.5f) {
        FinishCarouselMotion(state);
    }
}

void OnMotionTimer(lv_timer_t* timer) {
    auto* state = static_cast<HomeState*>(lv_timer_get_user_data(timer));
    if (state == nullptr || state->motion_timer != timer) return;
    const int64_t now_us = esp_timer_get_time();
    float elapsed_ms = static_cast<float>(now_us - state->motion_last_us) / 1000.0f;
    state->motion_last_us = now_us;
    elapsed_ms = std::clamp(elapsed_ms, 1.0f, 32.0f);

    if (state->snapping) {
        state->snap_elapsed_ms += elapsed_ms;
        const float t = std::min(1.0f, state->snap_elapsed_ms / kSnapDurationMs);
        // A small position-only overshoot reproduces the released-detent feel
        // without transform scale, rotation, or animated opacity.
        const float spring = (1.0f - t) * (1.0f - 1.35f * t);
        state->carousel_offset = state->snap_start_offset * spring;
        state->carousel_position = state->snap_target_position -
                                   state->carousel_offset / kCarouselStep;
        ApplyCarouselGeometry(state);
        if (t >= 1.0f) FinishCarouselMotion(state);
        return;
    }

    AdvanceCarousel(state, state->carousel_velocity * elapsed_ms);
    state->carousel_velocity *=
        std::pow(kFriction, elapsed_ms / 16.6667f);
    if (std::abs(state->carousel_velocity) < kStopVelocity) BeginSnap(state);
}

void StartCarouselMotion(HomeState* state, float velocity) {
    if (state == nullptr) return;
    ShowLiveCarousel(state);
    state->carousel_velocity = std::clamp(velocity, -kMaxVelocity, kMaxVelocity);
    state->snapping = false;
    if (std::abs(state->carousel_velocity) < kMinReleaseVelocity) {
        BeginSnap(state);
    }
    if (state->motion_timer == nullptr &&
        (state->snapping || std::abs(state->carousel_velocity) >= kMinReleaseVelocity)) {
        state->motion_last_us = esp_timer_get_time();
        state->motion_timer =
            lv_timer_create(OnMotionTimer, kMotionPeriodMs, state);
    }
}

void UpdateExpressionGaze(HomeState* state, lv_event_t* event) {
    if (state == nullptr || state->expression == nullptr) return;
    lv_indev_t* indev = lv_event_get_indev(event);
    if (indev == nullptr) indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    constexpr float kHalfDisplay = metrics::kDisplaySize / 2.0f;
    state->expression->SetLookAt(
        std::clamp((static_cast<float>(point.x) - kHalfDisplay) / kHalfDisplay,
                   -1.0f, 1.0f),
        std::clamp((static_cast<float>(point.y) - kHalfDisplay) / kHalfDisplay,
                   -1.0f, 1.0f));
}

void OnCarouselPressed(lv_event_t* event) {
    auto* state = static_cast<HomeState*>(lv_event_get_user_data(event));
    if (state == nullptr) return;
    DeleteMotionTimer(state);
    ShowLiveCarousel(state);
    lv_indev_t* indev = lv_event_get_indev(event);
    if (indev == nullptr) indev = lv_indev_active();
    if (indev != nullptr) {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        state->pointer_start_x = point.x;
        state->pointer_last_x = point.x;
        state->pointer_last_us = esp_timer_get_time();
        state->pointer_active = true;
        state->pointer_moved = false;
        state->suppress_click = false;
        state->carousel_velocity = 0.0f;
    }
    UpdateExpressionGaze(state, event);
}

void OnCarouselPressing(lv_event_t* event) {
    auto* state = static_cast<HomeState*>(lv_event_get_user_data(event));
    if (state == nullptr || !state->pointer_active) return;
    UpdateExpressionGaze(state, event);
    lv_indev_t* indev = lv_event_get_indev(event);
    if (indev == nullptr) indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const int64_t now_us = esp_timer_get_time();
    const int delta_x = point.x - state->pointer_last_x;
    const float elapsed_ms = std::max(
        1.0f, static_cast<float>(now_us - state->pointer_last_us) / 1000.0f);
    state->pointer_last_x = point.x;
    state->pointer_last_us = now_us;
    if (std::abs(point.x - state->pointer_start_x) >= kDragThreshold) {
        state->pointer_moved = true;
    }
    if (!state->pointer_moved) return;
    if (delta_x == 0) {
        state->carousel_velocity *= 0.72f;
        return;
    }
    const float instantaneous = delta_x / elapsed_ms;
    state->carousel_velocity =
        state->carousel_velocity * 0.68f + instantaneous * 0.32f;
    AdvanceCarousel(state, static_cast<float>(delta_x));
}

void OnCarouselReleased(lv_event_t* event) {
    auto* state = static_cast<HomeState*>(lv_event_get_user_data(event));
    if (state == nullptr) return;
    if (state->expression != nullptr) {
        state->expression->ClearLookAt();
    }
    if (!state->pointer_active) return;
    state->pointer_active = false;
    state->suppress_click = state->pointer_moved;
    StartCarouselMotion(
        state, state->pointer_moved ? state->carousel_velocity * kReleaseBoost
                                   : 0.0f);
}

void OpenAppFromCurrentCarouselPosition(HomeState* state, ScreenId screen) {
    if (state == nullptr || !state->actions.open_app) return;
    if (ShouldBlockForDisconnectedWifi(screen)) {
        ShowNetworkGuard(state);
        return;
    }
    s_saved_focused_index = state->focused_index;
    state->actions.open_app(screen);
}

void FinishWakeNavigation(void* user_data) {
    auto* state = static_cast<HomeState*>(user_data);
    if (state == nullptr || s_state != state) return;
    OpenAppFromCurrentCarouselPosition(state, state->pending_screen);
}

void OnOpenApp(lv_event_t* event) {
    auto* state = static_cast<HomeState*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (state == nullptr || target == nullptr || !state->actions.open_app) return;
    if (state->suppress_click) {
        state->suppress_click = false;
        return;
    }
    PlayHaptic(HapticStrength::Medium);
    const size_t index = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target));
    if (index >= state->app_definitions.size()) return;
    const AppDefinition& app = state->app_definitions[index];
    if (!app.external_id.empty() &&
        !external_apps::Manager::Get().Select(app.external_id)) {
        return;
    }
    const ScreenId screen = app.id;
    if (state->expression != nullptr &&
        (state->expression->IsSleeping() || state->expression->IsWaking())) {
        state->pending_screen = screen;
        state->expression->SetWakeCompletedCallback(FinishWakeNavigation, state);
        if (state->expression->IsSleeping()) state->expression->Wake();
        return;
    }
    OpenAppFromCurrentCarouselPosition(state, screen);
}

void OnExpressionClicked(lv_event_t* event) {
    auto* state = static_cast<HomeState*>(lv_event_get_user_data(event));
    if (state != nullptr && state->actions.toggle_listening) {
        // Wake first, then let the requested listening state queue behind the
        // wake animation. The touch must never be swallowed by expression sleep.
        IdlePower::Get().NotifyActivity();
        state->actions.toggle_listening();
    }
}

void OnDelete(lv_event_t* event) {
    auto* state = static_cast<HomeState*>(lv_event_get_user_data(event));
    if (state == nullptr) return;
    if (s_state == state) s_state = nullptr;
    if (state->conversation_exit_hit_area != nullptr &&
        lv_obj_is_valid(state->conversation_exit_hit_area)) {
        lv_obj_delete(state->conversation_exit_hit_area);
        state->conversation_exit_hit_area = nullptr;
    }
    DeleteMotionTimer(state);
    DeleteParallaxTimer(state);
    DeleteMessageTimer(state);
    DeleteConversationLayoutTimer(state);
    DeleteStandbyClockTimer(state);
    if (state->standby_hide_timer != nullptr) {
        lv_timer_delete(state->standby_hide_timer);
        state->standby_hide_timer = nullptr;
    }
    if (state->actions.unmounted) state->actions.unmounted();
    if (state->carousel_cache != nullptr) {
        lv_image_set_src(state->carousel_cache, nullptr);
    }
    if (state->carousel_snapshot != nullptr) {
        lv_image_cache_drop(state->carousel_snapshot);
        lv_draw_buf_destroy(state->carousel_snapshot);
        state->carousel_snapshot = nullptr;
    }
    if (state->left_fade != nullptr && state->left_fade_mask != nullptr) {
        lv_image_set_src(state->left_fade, nullptr);
    }
    if (state->right_fade != nullptr && state->right_fade_mask != nullptr) {
        lv_image_set_src(state->right_fade, nullptr);
    }
    if (state->left_fade_mask != nullptr) {
        lv_image_cache_drop(state->left_fade_mask);
        lv_draw_buf_destroy(state->left_fade_mask);
        state->left_fade_mask = nullptr;
    }
    if (state->right_fade_mask != nullptr) {
        lv_image_cache_drop(state->right_fade_mask);
        lv_draw_buf_destroy(state->right_fade_mask);
        state->right_fade_mask = nullptr;
    }
    delete state->expression;
    delete state;
}

ArcItem CreateArcItem(lv_obj_t* parent, const AppDefinition& app,
                      size_t app_index, HomeState* state) {
    const auto& colors = Theme::Get().colors();
    ArcItem item;
    item.button = lv_button_create(parent);
    lv_obj_remove_style_all(item.button);
    lv_obj_set_size(item.button, kCarouselItemWidth, 178);
    lv_obj_set_pos(item.button, kCarouselFocusX, 49);
    lv_obj_set_style_bg_opa(item.button, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_opa(item.button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(item.button, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(item.button, 12, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(item.button, 0, LV_PART_MAIN);
    lv_obj_set_user_data(item.button, reinterpret_cast<void*>(app_index));
    lv_obj_add_event_cb(item.button, OnCarouselPressed, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(item.button, OnCarouselPressing, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(item.button, OnCarouselReleased, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(item.button, OnCarouselReleased, LV_EVENT_PRESS_LOST, state);
    lv_obj_add_event_cb(item.button, OnOpenApp, LV_EVENT_CLICKED, state);

    item.rule = lv_obj_create(item.button);
    lv_obj_remove_style_all(item.rule);
    lv_obj_set_size(item.rule, 45, 3);
    lv_obj_set_pos(item.rule, 8, 0);
    lv_obj_set_style_bg_color(item.rule, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(item.rule, LV_OPA_COVER, LV_PART_MAIN);

    item.number = lv_label_create(item.button);
    lv_label_set_text(item.number, app.number.c_str());
    lv_obj_set_style_text_font(item.number, fonts::Large(), LV_PART_MAIN);
    lv_obj_set_style_text_color(item.number, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(item.number, 8, 13);

    item.name = lv_label_create(item.button);
    lv_label_set_text(item.name, app.name.c_str());
    const lv_font_t* regular_name_font = fonts::MediumBold();
    lv_point_t regular_name_size{};
    // Use rendered pixels instead of UTF-8 length: Latin glyph widths vary,
    // while a single CJK character occupies multiple bytes.
    lv_text_get_size(&regular_name_size, app.name.c_str(), regular_name_font,
                     0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const bool compact_name =
        regular_name_size.x > kCarouselNameWidth ||
        regular_name_size.y > regular_name_font->line_height;
    const lv_font_t* name_font =
        compact_name ? fonts::SmallBold() : regular_name_font;
    lv_obj_set_style_text_font(item.name, name_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(item.name, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_label_set_long_mode(item.name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(item.name, kCarouselNameWidth,
                    name_font->line_height * (compact_name ? 2 : 1));
    lv_obj_set_pos(item.name, kCarouselNameInset, kCarouselNameY);

    item.arrow = lv_label_create(item.button);
    lv_label_set_text(item.arrow, FONT_AWESOME_ARROW_RIGHT);
    lv_obj_set_style_text_font(item.arrow, fonts::Icon(), LV_PART_MAIN);
    lv_obj_set_style_text_color(item.arrow, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(item.arrow, 8,
                   compact_name ? kCarouselNameCompactArrowY : 120);
    return item;
}

lv_obj_t* CreateHalfDetent(lv_obj_t* parent) {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* detent = lv_obj_create(parent);
    lv_obj_remove_style_all(detent);
    lv_obj_set_size(detent, kCarouselDetentSize, kCarouselDetentSize);
    lv_obj_set_style_radius(detent, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(detent, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(detent, 70, LV_PART_MAIN);
    lv_obj_remove_flag(detent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(detent, LV_OBJ_FLAG_SCROLLABLE);
    return detent;
}

}  // namespace

lv_obj_t* Renderer::Create(RendererActions actions) {
    const auto& colors = Theme::Get().colors();
    auto* state = new HomeState{};
    state->actions = std::move(actions);
    state->app_definitions = BuildAppDefinitions();
    state->apps.resize(state->app_definitions.size());
    state->half_detents.resize(state->app_definitions.size());
    s_state = state;

    state->root = lv_obj_create(nullptr);
    StyleRoot(state->root);
    lv_obj_add_event_cb(state->root, OnDelete, LV_EVENT_DELETE, state);

    lv_obj_t* expression = lv_obj_create(state->root);
    lv_obj_remove_style_all(expression);
    // A 600x400 cropped surface preserves the 1.5x expression scale without
    // allocating the unused top and bottom of a full 600x600 A8 buffer.
    lv_obj_set_size(expression, 600, 400);
    lv_obj_set_pos(expression, 60, kHeroTop + 48);
    lv_obj_remove_flag(expression, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(expression, LV_OBJ_FLAG_SCROLLABLE);
    state->expression_parallax.object = expression;
    state->expression = new ExpressionPlayer(
        expression, &Application::GetInstance().GetAudioService()
                          .GetListeningAudioFeatures());

    state->message_viewport = lv_obj_create(state->root);
    lv_obj_remove_style_all(state->message_viewport);
    lv_obj_set_size(state->message_viewport, kMessageWidth,
                    MessageViewportHeight());
    lv_obj_set_pos(state->message_viewport, 34, kHeroTop + 28);
    lv_obj_remove_flag(state->message_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state->message_viewport, LV_OBJ_FLAG_CLICKABLE);
    state->message_parallax.object = state->message_viewport;

    state->message = lv_label_create(state->message_viewport);
    lv_label_set_text(state->message, "我在，\n随时可以开始。");
    lv_obj_set_width(state->message, kMessageWidth);
    lv_obj_set_height(state->message, LV_SIZE_CONTENT);
    lv_label_set_long_mode(state->message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(state->message, fonts::LargeBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(state->message, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(state->message, kMessageLineSpace,
                                     LV_PART_MAIN);
    lv_obj_set_pos(state->message, 0, 0);

    state->editorial_rule = lv_obj_create(state->root);
    lv_obj_remove_style_all(state->editorial_rule);
    lv_obj_set_size(state->editorial_rule, 58, 4);
    lv_obj_set_pos(state->editorial_rule, 35, kHeroTop + 190);
    lv_obj_set_style_bg_color(state->editorial_rule,
                              lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->editorial_rule, LV_OPA_COVER, LV_PART_MAIN);
    state->editorial_parallax.object = state->editorial_rule;

    state->listening_hit_area = lv_obj_create(state->root);
    lv_obj_remove_style_all(state->listening_hit_area);
    lv_obj_set_size(state->listening_hit_area, metrics::kDisplaySize,
                    kCarouselTop - kHeroTop);
    lv_obj_set_pos(state->listening_hit_area, 0, kHeroTop);
    lv_obj_add_flag(state->listening_hit_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(state->listening_hit_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(state->listening_hit_area, OnExpressionClicked,
                        LV_EVENT_CLICKED, state);

    state->conversation_exit_hit_area = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(state->conversation_exit_hit_area);
    lv_obj_set_size(state->conversation_exit_hit_area, metrics::kDisplaySize,
                    metrics::kDisplaySize);
    lv_obj_set_pos(state->conversation_exit_hit_area, 0, 0);
    lv_obj_set_style_bg_opa(state->conversation_exit_hit_area, LV_OPA_TRANSP,
                            LV_PART_MAIN);
    lv_obj_add_flag(state->conversation_exit_hit_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(state->conversation_exit_hit_area,
                       LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(state->conversation_exit_hit_area,
                        OnExpressionClicked, LV_EVENT_CLICKED, state);
    lv_obj_add_flag(state->conversation_exit_hit_area, LV_OBJ_FLAG_HIDDEN);

    state->carousel = lv_obj_create(state->root);
    lv_obj_remove_style_all(state->carousel);
    lv_obj_set_size(state->carousel, metrics::kDisplaySize, kCarouselHeight);
    lv_obj_set_pos(state->carousel, 0, kCarouselTop);
    lv_obj_remove_flag(state->carousel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(state->carousel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(state->carousel, lv_color_hex(colors.background),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->carousel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(state->carousel, OnCarouselPressed, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(state->carousel, OnCarouselPressing, LV_EVENT_PRESSING,
                        state);
    lv_obj_add_event_cb(state->carousel, OnCarouselReleased, LV_EVENT_RELEASED,
                        state);
    lv_obj_add_event_cb(state->carousel, OnCarouselReleased, LV_EVENT_PRESS_LOST,
                        state);
    state->carousel_parallax[0].object = state->carousel;

    lv_obj_t* track = lv_obj_create(state->carousel);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, metrics::kDisplaySize, kCarouselHeight);
    lv_obj_set_pos(track, 0, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

    for (size_t index = 0; index < state->app_definitions.size(); ++index) {
        state->half_detents[index] = CreateHalfDetent(track);
    }
    for (size_t index = 0; index < state->app_definitions.size(); ++index) {
        state->apps[index] =
            CreateArcItem(track, state->app_definitions[index], index, state);
    }

    state->left_fade = CreateFadeMask(state, true);
    state->right_fade = CreateFadeMask(state, false);

    state->carousel_cache = lv_image_create(state->root);
    lv_obj_set_pos(state->carousel_cache, 0, kCarouselTop);
    lv_obj_remove_flag(state->carousel_cache, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(state->carousel_cache, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(state->carousel_cache, LV_OBJ_FLAG_HIDDEN);
    state->carousel_parallax[1].object = state->carousel_cache;
    state->carousel_parallax[2].object = state->left_fade;
    state->carousel_parallax[3].object = state->right_fade;
    lv_obj_move_background(state->carousel_cache);
    lv_obj_move_foreground(state->carousel);
    lv_obj_move_foreground(state->left_fade);
    lv_obj_move_foreground(state->right_fade);

    state->standby_time = lv_label_create(state->root);
    lv_label_set_text(state->standby_time, "00:00");
    lv_obj_set_style_text_font(state->standby_time, fonts::LargeBold(),
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(state->standby_time,
                                lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(state->standby_time, metrics::kSystemPadding, 548);
    lv_obj_set_style_translate_y(state->standby_time, kStandbyBottomOffset,
                                 LV_PART_MAIN);
    lv_obj_add_flag(state->standby_time, LV_OBJ_FLAG_HIDDEN);

    state->standby_date = lv_label_create(state->root);
    lv_label_set_text(state->standby_date, "--月--日");
    lv_obj_set_style_text_font(state->standby_date, fonts::MediumBold(),
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(state->standby_date,
                                lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(state->standby_date, metrics::kSystemPadding, 612);
    lv_obj_set_style_translate_y(state->standby_date, kStandbyBottomOffset,
                                 LV_PART_MAIN);
    lv_obj_add_flag(state->standby_date, LV_OBJ_FLAG_HIDDEN);

    state->standby_rule = lv_obj_create(state->root);
    lv_obj_remove_style_all(state->standby_rule);
    lv_obj_set_size(state->standby_rule, 58, 4);
    lv_obj_set_pos(state->standby_rule, metrics::kSystemPadding, 674);
    lv_obj_set_style_bg_color(state->standby_rule,
                              lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->standby_rule, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_translate_y(state->standby_rule, kStandbyBottomOffset,
                                 LV_PART_MAIN);
    lv_obj_add_flag(state->standby_rule, LV_OBJ_FLAG_HIDDEN);

    ApplyFocus(state, s_saved_focused_index);
    ApplyCarouselGeometry(state);
    CacheSettledCarousel(state);
    StartParallaxTimer(state);
    return state->root;
}

void Renderer::RefreshAi(AgentState state, const char* message,
                         bool conversation_message, bool user_message) {
    if (s_state == nullptr || s_state->root == nullptr) return;
    if (s_state->expression != nullptr) s_state->expression->SetState(state);
    AnimateConversationLayout(s_state, state != AgentState::Idle);
    if (s_state->message != nullptr && message != nullptr && message[0] != '\0') {
        const std::string display_text =
            conversation_message ? FormatConversationText(message) : message;
        const bool text_changed = s_state->rendered_message != display_text;
        lv_obj_set_style_text_font(s_state->message, fonts::LargeBold(), LV_PART_MAIN);
        const auto& colors = Theme::Get().colors();
        lv_obj_set_style_text_color(
            s_state->message,
            lv_color_hex(user_message ? colors.accent : colors.text), LV_PART_MAIN);
        lv_obj_set_style_text_line_space(s_state->message, kMessageLineSpace,
                                         LV_PART_MAIN);
        lv_obj_set_height(s_state->message, LV_SIZE_CONTENT);
        lv_label_set_long_mode(s_state->message, LV_LABEL_LONG_WRAP);
        if (!text_changed) return;
        s_state->rendered_message = display_text;
        lv_label_set_text(s_state->message, display_text.c_str());
        lv_obj_update_layout(s_state->message);
        if (!s_state->standby) StartMessageScroll(s_state);
    }
}

void Renderer::NotifyUserActivity() {
    if (s_state == nullptr || s_state->expression == nullptr) return;
    if (s_state->expression->IsSleeping()) s_state->expression->Wake();
}

void Renderer::SleepExpression() {
    if (s_state == nullptr || s_state->expression == nullptr) return;
    s_state->expression->Sleep();
}

void Renderer::EnterStandby() {
    if (s_state == nullptr || s_state->expression == nullptr ||
        s_state->standby) {
        return;
    }
    s_state->standby = true;
    // Enter standby directly from whichever Home layout is visible. The
    // queued ForceReturnToIdle event must update content only; it must not
    // animate the regular Home layout back onscreen underneath standby.
    s_state->conversation_active = false;
    DeleteConversationLayoutTimer(s_state);
    SetConversationExitHitArea(s_state, false);
    DeleteMotionTimer(s_state);
    DeleteParallaxTimer(s_state);
    ResetParallax(s_state);
    DeleteMessageTimer(s_state);
    SetCarouselInteractive(s_state, false);
    s_state->expression->ClearLookAt();
    s_state->expression->Sleep();
    if (s_state->listening_hit_area != nullptr) {
        lv_obj_remove_flag(s_state->listening_hit_area, LV_OBJ_FLAG_CLICKABLE);
    }
    AnimateHomeChrome(s_state, false);
    AnimateStandbyChrome(s_state, true);
}

void Renderer::ExitStandby(WakeCompletedCallback callback, void* user_data) {
    if (s_state == nullptr || s_state->expression == nullptr) {
        if (callback != nullptr) lv_async_call(callback, user_data);
        return;
    }
    s_state->standby = false;
    StartParallaxTimer(s_state);
    // Standby always wakes to the regular Home layout, even if the carousel
    // had already been hidden after entering the minimal conversation layout.
    s_state->conversation_active = false;
    DeleteConversationLayoutTimer(s_state);
    SetConversationExitHitArea(s_state, false);
    SetCarouselRenderingHidden(s_state, false);
    SetCarouselInteractive(s_state, false);
    AnimateHomeChrome(s_state, true);
    AnimateStandbyChrome(s_state, false);
    ScheduleConversationLayoutFinish(s_state, kStandbyChromeDurationMs);
    StartMessageScroll(s_state);
    if (s_state->listening_hit_area != nullptr) {
        lv_obj_add_flag(s_state->listening_hit_area, LV_OBJ_FLAG_CLICKABLE);
    }
    s_state->expression->SetWakeCompletedCallback(callback, user_data);
    if (s_state->expression->IsSleeping()) {
        s_state->expression->Wake();
    } else if (!s_state->expression->IsWaking() && callback != nullptr) {
        lv_async_call(callback, user_data);
    }
}

void Renderer::SetRenderingPaused(bool paused) {
    if (s_state == nullptr || s_state->expression == nullptr) return;
    s_state->expression->SetRenderingPaused(paused);
    if (paused) {
        DeleteParallaxTimer(s_state);
        ResetParallax(s_state);
        DeleteStandbyClockTimer(s_state);
    } else {
        if (s_state->standby) {
            StartStandbyClockTimer(s_state);
        } else {
            StartParallaxTimer(s_state);
        }
    }
}

lv_obj_t* Renderer::Screen() {
    return s_state != nullptr ? s_state->root : nullptr;
}

void Renderer::UpdateBattery(bool has_battery, int level, bool charging) {
    if (s_state == nullptr || s_state->expression == nullptr || !has_battery) {
        return;
    }
    const ExpressionPlayer::Energy energy =
        level < 20 ? ExpressionPlayer::Energy::Exhausted
                   : (level < 50 ? ExpressionPlayer::Energy::Tired
                                 : ExpressionPlayer::Energy::Normal);
    s_state->expression->SetEnergy(energy);

    // Keep the last active-screen charging edge while the panel is in
    // low-power standby. Charger status can be temporarily unavailable there,
    // and accepting that false edge would announce the same cable again when
    // the device wakes.
    if (Application::GetInstance().IsLowPowerStandby()) {
        return;
    }

    const uint32_t now = lv_tick_get();
    if (!s_state->battery_initialized) {
        // An unplugged first sample establishes the baseline. If external
        // power is already present when the UI starts, keep the confirmed
        // state false so the normal stability window announces charging once.
        s_state->charging = false;
        s_state->charging_candidate = charging;
        s_state->charging_candidate_since_tick = now;
        s_state->battery_initialized = true;
        if (!charging) {
            return;
        }
    }

    if (charging != s_state->charging_candidate) {
        s_state->charging_candidate = charging;
        s_state->charging_candidate_since_tick = now;
        return;
    }
    if (charging == s_state->charging) {
        return;
    }

    // Require a short stable VBUS assertion before announcing a connection,
    // and a longer stable removal before re-arming. The asymmetric release
    // window prevents a brief I2C/VBUS dropout from creating a second prompt
    // during the same physical cable session.
    constexpr uint32_t kChargingConnectConfirmMs = 1500;
    constexpr uint32_t kChargingDisconnectConfirmMs = 5000;
    const uint32_t confirm_ms = charging ? kChargingConnectConfirmMs
                                         : kChargingDisconnectConfirmMs;
    if (lv_tick_elaps(s_state->charging_candidate_since_tick) < confirm_ms) {
        return;
    }

    s_state->charging = charging;
    if (charging) {
        // Charging can begin while the expression is in standby sleep. Wake
        // first so the charging action is rendered instead of only queued.
        if (s_state->expression->IsSleeping()) {
            s_state->expression->Wake();
        }
        s_state->expression->PlayCharging();
    }
}

void Renderer::PlayDizzy() {
    if (s_state == nullptr || s_state->expression == nullptr) return;
    if (s_state->expression->IsSleeping()) s_state->expression->Wake();
    s_state->expression->PlayDizzy();
}

void Renderer::HoldChargingExpression() {
    if (s_state == nullptr || s_state->expression == nullptr) return;
    s_state->expression->HoldCharging();
}

void Renderer::HoldDizzyExpression() {
    if (s_state == nullptr || s_state->expression == nullptr) return;
    s_state->expression->HoldDizzy();
}

void Renderer::ReleaseSpecialExpression() {
    if (s_state == nullptr || s_state->expression == nullptr) return;
    s_state->expression->ReleaseSpecialExpression();
}

bool Renderer::IsMounted() {
    return s_state != nullptr && s_state->root != nullptr;
}

}  // namespace agent_ui::home
