#include "codex_view.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

#include <esp_log.h>
#include <esp_random.h>
#include <font_awesome.h>
#include <mbedtls/base64.h>

#include "cJSON.h"
#include "codex_ws_client.h"
#include "codex_ws_endpoint.h"
#include "codex_bridge_connection_state.h"
#include "codex_menu_state.h"
#include "codex_menu_ui.h"
#include "codex_conversation_ui.h"
#include "codex_notification_service.h"
#include "codex_status_ring.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "core/app_shell.h"
#include "core/fonts.h"
#include "core/idle_power.h"
#include "core/theme.h"
#include "ui_dispatcher.h"
#include "components/system_keyboard.h"
#include "components/ui_components.h"


namespace agent_ui {
namespace {

namespace controls = ui_components;

constexpr char kTag[] = "AgentCodex";
constexpr int kMaxMessages = 10;
constexpr int kUserBubbleWidth = 590;
constexpr int kAssistantBubbleWidth = 620;
// A press immediately requests voice input; capture waits for the PC's ready
// reply. The destructive stop action uses a deliberately slower hold gesture.
constexpr uint32_t kStopHoldDurationMs = 1200;
constexpr uint32_t kStopHoldUpdateMs = 20;

enum class VoiceStage {
    Idle,
    Preparing,
    Recording,
    Finishing,
    AwaitingRecognition,
    Recognizing,
    Submitting,
    Error,
};

enum class VoiceMode { Micro, Api };
enum class RealtimeVisual { Connecting, Listening, Thinking, Speaking, Muted, Error };
enum class MenuTab : uint8_t { Tasks, Model, Connection };

struct UiState {
    std::atomic<lv_obj_t*> root{nullptr};
    lv_obj_t* chat = nullptr;
    lv_obj_t* task_actions = nullptr;
    std::unique_ptr<CodexConversationUi> conversation_ui;
    lv_obj_t* action_button = nullptr;
    lv_obj_t* action_icon = nullptr;
    lv_obj_t* action_label = nullptr;
    lv_obj_t* stop_button = nullptr;
    lv_obj_t* stop_label = nullptr;
    controls::VoiceButtonParts voice_button{};
    lv_obj_t* stop_hold_progress = nullptr;
    lv_timer_t* stop_hold_timer = nullptr;
    lv_obj_t* config_overlay = nullptr;
    lv_obj_t* discovery_name = nullptr;
    lv_obj_t* connection_modes[2]{};
    lv_obj_t* connection_panels[2]{};
    controls::ActionButtonParts connection_action{};
    codex_menu_ui::Parts menu_ui{};
    lv_obj_t* remote_ip = nullptr;
    lv_obj_t* token = nullptr;
    lv_obj_t* menu_tabs[3]{};
    lv_obj_t* menu_panels[3]{};
    lv_obj_t* task_cards[6]{};
    lv_obj_t* new_task_button = nullptr;
    lv_obj_t* realtime_button = nullptr;
    lv_obj_t* model_summary = nullptr;
    lv_obj_t* model_dropdown = nullptr;
    lv_obj_t* fast_switch = nullptr;
    lv_obj_t* ring_frame = nullptr;
    lv_timer_t* menu_pending_timer = nullptr;
    lv_timer_t* voice_label_timer = nullptr;
    lv_timer_t* bridge_status_timer = nullptr;
    std::string discovered_name;
    std::string discovered_ip;
    std::string voice_request_id;
    std::string realtime_request_id;
    std::string realtime_host_id;
    std::string realtime_thread_id;
    std::string realtime_stream_id;
    uint32_t realtime_generation = 0;
    uint32_t realtime_last_sequence = 0;
    bool realtime_capture_started = false;
    lv_obj_t* realtime_root = nullptr;
    lv_obj_t* realtime_canvas = nullptr;
    lv_obj_t* realtime_status = nullptr;
    lv_obj_t* realtime_mute = nullptr;
    lv_obj_t* realtime_volume = nullptr;
    lv_obj_t* realtime_end = nullptr;
    lv_timer_t* realtime_timer = nullptr;
    RealtimeVisual realtime_visual = RealtimeVisual::Connecting;
    bool realtime_muted = false;
    int realtime_volume_percent = 70;
    bool pending_new_task = false;
    std::string stop_request_id;
    std::string stop_target;
    std::string voice_target;
    std::string connected_token;
    std::string connected_remote_ip;
    int discovered_port = 8765;
    uint32_t request_counter = 0;
    uint32_t menu_boot_nonce = esp_random();
    uint32_t stop_hold_started_at = 0;
    VoiceStage voice_stage = VoiceStage::Idle;
    VoiceMode voice_mode = VoiceMode::Micro;
    bool voice_pressed = false;
    bool voice_cancel_armed = false;
    bool voice_cancel_pending = false;
    int32_t voice_press_y = 0;
    bool voice_capture_started = false;
    bool task_active = false;
    bool stop_pending = false;
    bool stop_pressed = false;
    bool stop_triggered = false;
    bool remote_mode = false;
    bool connected_remote_mode = false;
    bool voice_animation_active = false;
    bool ring_enabled = true;
    bool bridge_was_usable = false;
    std::string displayed_task;
    std::vector<codex_menu::Message> displayed_messages;
    bool displayed_ready = false;
    bool conversation_initialized = false;
    std::string pending_menu_request;
    codex_menu::State menu_state{};
    codex_bridge::ConnectionState bridge_connection{};
};

UiState s_ui;
codex_menu::TaskEntrySelection s_task_entry;

bool IsWorkingState(const char* state) {
    return std::strcmp(state, "working") == 0 ||
           std::strcmp(state, "in_progress") == 0 ||
           std::strcmp(state, "running") == 0 ||
           std::strcmp(state, "active") == 0;
}

bool IsIdleState(const char* state) {
    return std::strcmp(state, "idle") == 0 ||
           std::strcmp(state, "completed") == 0 ||
           std::strcmp(state, "stopped") == 0 ||
           std::strcmp(state, "failed") == 0 ||
           std::strcmp(state, "error") == 0 ||
           std::strcmp(state, "cancelled") == 0 ||
           std::strcmp(state, "canceled") == 0;
}

bool CanUseCodex() {
    return s_ui.bridge_connection.CanUseCodex(lv_tick_get());
}

void SetStatus(bool connected, const char* text = nullptr) {
    codex_menu_ui::SetConnectionStatus(s_ui.menu_ui, connected,
        text != nullptr ? text : (connected ? "已连接" : "未连接"));
}

void UpdateActionButton() {
    if (s_ui.action_button == nullptr || s_ui.action_icon == nullptr) return;
    const bool codex_available = CanUseCodex();
    const auto& colors = Theme::Get().colors();
    uint32_t color = colors.accent;
    const char* icon = FONT_AWESOME_MICROPHONE;
    if (s_ui.voice_stage != VoiceStage::Idle) {
        color = colors.warning;
        icon = s_ui.voice_stage == VoiceStage::Recording
                   ? FONT_AWESOME_STOP
                   : FONT_AWESOME_MICROPHONE;
    }
    if (s_ui.voice_cancel_armed) color = colors.danger;
    lv_obj_set_style_bg_color(s_ui.action_button, lv_color_hex(color), LV_PART_MAIN);
    // Cancel is a destructive gesture: make it distinct from the normal
    // recording warning fill even in themes where their luminance is close.
    const bool cancelling = s_ui.voice_cancel_armed && s_ui.voice_pressed;
    lv_obj_set_style_border_width(s_ui.action_button, cancelling ? 3 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.action_button, lv_color_hex(cancelling ? 0xFFFFFF : color), LV_PART_MAIN);
    const auto pressed = static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_ui.action_button, lv_color_hex(color), pressed);
    lv_obj_set_style_border_width(s_ui.action_button, cancelling ? 3 : 0, pressed);
    lv_obj_set_style_border_color(s_ui.action_button, lv_color_hex(cancelling ? 0xFFFFFF : color), pressed);
    if (s_ui.action_label) lv_obj_set_style_text_color(s_ui.action_label,
        lv_color_hex(cancelling ? 0x7F1D1D : colors.accent_ink), LV_PART_MAIN);
    if (s_ui.action_icon) lv_obj_set_style_text_color(s_ui.action_icon,
        lv_color_hex(cancelling ? 0x7F1D1D : colors.accent_ink), LV_PART_MAIN);
    lv_label_set_text(s_ui.action_icon, icon);
    if (s_ui.action_label != nullptr) {
        const char* label = "按住说话";
        if (s_ui.voice_cancel_armed && s_ui.voice_pressed) {
            label = "松开取消";
        } else if (s_ui.voice_cancel_pending) {
            label = "取消中…";
        } else if (codex_available && s_ui.voice_stage == VoiceStage::Preparing) {
            label = "等待…";
        } else if (s_ui.voice_stage == VoiceStage::AwaitingRecognition) {
            label = "请稍后...";
        } else if (s_ui.voice_stage == VoiceStage::Recording) {
            label = "松开发送";
        } else if (s_ui.voice_stage == VoiceStage::Finishing) {
            label = "结束中...";
        } else if (s_ui.voice_stage == VoiceStage::Recognizing) {
            label = "转录中...";
        } else if (s_ui.voice_stage == VoiceStage::Submitting) {
            label = s_ui.task_active ? "正在引导..." : "正在发送...";
        } else if (s_ui.voice_stage == VoiceStage::Error) {
            label = "发送失败";
        } else if (s_ui.task_active) {
            label = "按住说话";
        }
        lv_label_set_text(s_ui.action_label, label);
    }
    if (codex_available && s_ui.voice_pressed) {
        SetStatus(true, s_ui.voice_cancel_armed ? "松开手指，取消本次输入" : "松开发送 · 上滑取消");
    }
    if (codex_available) lv_obj_remove_state(s_ui.action_button, LV_STATE_DISABLED);
    else lv_obj_add_state(s_ui.action_button, LV_STATE_DISABLED);
    const bool animate_voice =
        s_ui.voice_stage != VoiceStage::Idle &&
        s_ui.voice_stage != VoiceStage::Error;
    if (s_ui.voice_animation_active != animate_voice) {
        controls::SetVoiceButtonAnimating(s_ui.voice_button, animate_voice);
        s_ui.voice_animation_active = animate_voice;
    }
    if (s_ui.stop_button != nullptr) {
        if (codex_available && s_ui.task_active) lv_obj_remove_flag(s_ui.stop_button, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_ui.stop_button, LV_OBJ_FLAG_HIDDEN);
        if (!codex_available || s_ui.voice_stage != VoiceStage::Idle || s_ui.stop_pending)
            lv_obj_add_state(s_ui.stop_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(s_ui.stop_button, LV_STATE_DISABLED);
    }
    if (s_ui.stop_label != nullptr) lv_label_set_text(s_ui.stop_label,
        s_ui.stop_pending ? "正在停止…" : s_ui.stop_pressed ? "继续按住…" : "停止");
    // Both primary actions share the available width beside the menu.
    if (s_ui.voice_button.record_dot != nullptr) {
        auto* signal = lv_obj_get_parent(s_ui.voice_button.record_dot);
        if (s_ui.task_active) lv_obj_add_flag(signal, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(signal, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(s_ui.action_icon, LV_ALIGN_LEFT_MID, s_ui.task_active ? 16 : 34, 0);
    lv_obj_align(s_ui.action_label, LV_ALIGN_CENTER, s_ui.task_active ? 18 : 0, 0);
}

void UpdateStatusRing();

void SetVoiceStage(VoiceStage stage) {
    s_ui.voice_stage = stage;
    codex_notification::Get().SetRecording(stage != VoiceStage::Idle && stage != VoiceStage::Error);
    UpdateActionButton();
    UpdateStatusRing();
}

void CancelVoiceLabelTimer();
void ResetStopHoldProgress();
void FinishVoiceCapture(bool cancelled = false);
void TrySelectEntryTask();

std::string CurrentVoiceTarget() {
    const auto* target = codex_menu::SelectedTask(s_ui.menu_state);
    return s_ui.menu_state.stream_id + ":" + (target ? target->host_id + ":" + target->thread_id :
        "draft:" + s_ui.menu_state.draft_request_id);
}

std::string TargetedRequest(const char* type, const std::string& request_id) {
    const auto& state = s_ui.menu_state;
    const auto* target = codex_menu::SelectedTask(state);
    if (!state.connected || state.stream_id.empty() ||
        (!target && state.draft_request_id.empty())) return {};
    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "type", type);
    cJSON_AddStringToObject(request, "requestId", request_id.c_str());
    cJSON_AddStringToObject(request, "stream_id", state.stream_id.c_str());
    cJSON_AddStringToObject(request, "host_id", target ? target->host_id.c_str() : "local");
    cJSON_AddStringToObject(request, "thread_id", target ? target->thread_id.c_str() : "");
    cJSON_AddStringToObject(request, "draft_id", target ? "" : state.draft_request_id.c_str());
    char* json = cJSON_PrintUnformatted(request);
    const std::string result = json ? json : "";
    if (json) cJSON_free(json);
    cJSON_Delete(request);
    return result;
}
void RefreshCodexMenu(bool force = false);
void RefreshDraftSettings(bool require_model_tab = true);
void UpdateStatusRing();
void OnMenuTab(lv_event_t* event);
void OnTaskSlot(lv_event_t* event);
void OnMenuNewTask(lv_event_t* event);
void OnMenuRealtime(lv_event_t* event);
void OnModelDropdown(lv_event_t* event);
void OnEffortReleased(lv_event_t* event);
void OnFastChanged(lv_event_t* event);
void OnRingChanged(lv_event_t* event);
void SetMenuTab(MenuTab tab);
void RefreshBridgeUi(uint32_t now_ms);
bool MenuVoiceBusy();
void HideConfig();

void StopDeviceVoiceCapture(std::function<void()> on_stopped = {}) {
    if (s_ui.voice_capture_started) {
        s_ui.voice_capture_started = false;
        Application::GetInstance().StopCodexVoiceCapture(std::move(on_stopped));
    } else if (on_stopped) {
        on_stopped();
    }
}

bool IsCurrentRealtime(const cJSON* root) {
    const cJSON* request = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    const cJSON* host = cJSON_GetObjectItemCaseSensitive(root, "host_id");
    const cJSON* thread = cJSON_GetObjectItemCaseSensitive(root, "thread_id");
    const cJSON* stream = cJSON_GetObjectItemCaseSensitive(root, "stream_id");
    return cJSON_IsString(request) && cJSON_IsString(host) && cJSON_IsString(thread) &&
           cJSON_IsString(stream) && !s_ui.realtime_request_id.empty() &&
           s_ui.realtime_request_id == request->valuestring &&
           s_ui.realtime_host_id == host->valuestring &&
           s_ui.realtime_thread_id == thread->valuestring &&
           s_ui.realtime_stream_id == stream->valuestring;
}

void RenderRealtimePixels(lv_event_t* event) {
    auto* target = lv_event_get_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(target, &area);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    const auto& colors = Theme::Get().colors();
    dsc.bg_color = lv_color_hex(colors.accent);
    // Match the demo: 56 cells across 600 px, each square exactly 7 px.
    // Geometry is computed once; the small central motif uses only nearby cells.
    struct Pixel { int x, y; float dx, dy, r, angle; };
    static const std::vector<Pixel> pixels = [] {
        std::vector<Pixel> result;
        const float scale = 48.0f / 56.0f * (600.0f / 288.0f);
        for (int gy = 0; gy < 56; ++gy) for (int gx = 0; gx < 56; ++gx) {
            const float dx = (gx - 27.5f) * scale, dy = (gy - 27.5f) * scale;
            const float r = std::hypot(dx, dy);
            if (r > 26) continue;
            result.push_back({static_cast<int>(std::floor((gx + .5f) * 600 / 56 - 3.5f)),
                              static_cast<int>(std::floor((gy + .5f) * 600 / 56 - 3.5f)) - 85,
                              dx, dy, r, std::atan2(dy, dx)});
        }
        return result;
    }();
    const float t = static_cast<float>(lv_tick_get()) / 1000.0f;
    const float edge = 18 + std::round(std::sin(t * 1.7f));
    const float scan = 22 - std::fmod(t * 5, 20.0f);
    const float sweep = std::sin(t * 1.6f) * 10;
    for (const auto& pixel : pixels) {
        const float dx = pixel.dx, dy = pixel.dy, r = pixel.r, angle = pixel.angle;
        float light = 0.0f;
        if (s_ui.realtime_visual == RealtimeVisual::Listening) {
            if (r < edge) light = .22f + .35f * (1 - r / edge);
            if (std::abs(r - edge) < .8f || (std::abs(r - scan) < 1 && r < edge) ||
                (std::abs(dy - sweep) < 1 && r < edge - 2)) light = 1;
        } else if (s_ui.realtime_visual == RealtimeVisual::Thinking) {
            if (r > 5 && r < 21) { float strand=(std::cos((angle+t*1.8f+r*.17f)*3)+1)/2; light=strand>.88f?1:strand>.65f?.45f:.08f; }
            if (std::abs(dx)<=2 && std::abs(dy)<=2) light=.85f;
        } else if (s_ui.realtime_visual == RealtimeVisual::Speaking) {
            const float crest=16+std::round(2*std::sin(angle*5+t*7)+std::sin(angle*9-t*4));
            if (std::abs(r-crest)<1.5f) light=1;
            if (std::abs(r-(18+std::fmod(t*8,7.0f)))<.7f) light=.3f;
            const int col=static_cast<int>(std::floor((dx+10)/4)); const int h=2+static_cast<int>(std::floor((std::sin(col*1.7f+t*8)+1)*3));
            if (std::abs(dx)<10 && std::abs(dy)<=h && (static_cast<int>(std::floor(dx+10))%4)<2) light=1;
        } else if (s_ui.realtime_visual == RealtimeVisual::Connecting) {
            const int sector=static_cast<int>(std::floor((angle+static_cast<float>(M_PI))/(static_cast<float>(M_PI)/4))); const int head=static_cast<int>(t*8)%8;
            if (r > 16 && r < 19) light = sector == head ? 1 : sector == (head + 7) % 8 ? .5f : .12f;
            if (r < 2) light = .8f;
        } else if (s_ui.realtime_visual == RealtimeVisual::Muted) { dsc.bg_color=lv_color_hex(colors.muted); if(std::abs(r-18)<1)light=.25f; if(std::abs(dy)<7&&((dx>=-6&&dx<=-3)||(dx>=3&&dx<=6)))light=.8f; }
        else { dsc.bg_color=lv_color_hex(colors.danger); if(std::abs(r-18)<1&&std::abs(dy)>3)light=.3f; if(std::abs(dx)<7&&std::abs(dy)<7&&std::abs(std::abs(dx)-std::abs(dy))<1.1f)light=1; }
        if (light <= 0) continue;
        dsc.bg_opa=static_cast<lv_opa_t>(std::round(light*255));
        const int x = area.x1 + pixel.x, y = area.y1 + pixel.y;
        lv_area_t cell{x,y,x+6,y+6}; lv_draw_rect(lv_event_get_layer(event),&dsc,&cell);
    }
}

void SetRealtimeVisual(RealtimeVisual visual, const char* label) {
    s_ui.realtime_visual = visual;
    if (s_ui.realtime_status != nullptr) lv_label_set_text(s_ui.realtime_status, label);
    if (s_ui.realtime_canvas != nullptr) lv_obj_invalidate(s_ui.realtime_canvas);
}

void ShowRealtimePage(bool show) {
    if (s_ui.realtime_root == nullptr) return;
    if (show) {
        if (s_ui.task_actions) lv_obj_add_flag(s_ui.task_actions, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.chat, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.action_button) lv_obj_add_flag(s_ui.action_button, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.stop_button) lv_obj_add_flag(s_ui.stop_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.realtime_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_ui.task_actions) lv_obj_remove_flag(s_ui.task_actions, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.realtime_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.chat, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.action_button) lv_obj_remove_flag(s_ui.action_button, LV_OBJ_FLAG_HIDDEN);
        UpdateActionButton();
    }
}

void EndRealtimeSession(bool notify_pc) {
    const std::string request_id = s_ui.realtime_request_id;
    s_ui.realtime_capture_started = false;
    s_ui.realtime_request_id.clear();
    s_ui.realtime_host_id.clear();
    s_ui.realtime_thread_id.clear();
    s_ui.realtime_stream_id.clear();
    s_ui.realtime_generation = 0;
    s_ui.realtime_last_sequence = 0;
    Application::GetInstance().EndCodexRealtimeSession();
    ShowRealtimePage(false);
    if (notify_pc && !request_id.empty() && CodexWsClient::GetInstance().IsConnected()) {
        CodexWsClient::GetInstance().SendTextMessage(
            "{\"type\":\"realtime_end\",\"requestId\":\"" + request_id + "\"}",
            pdMS_TO_TICKS(200));
    }
}

void StartRealtimeSession() {
    if (!CanUseCodex() || !s_ui.realtime_request_id.empty() || MenuVoiceBusy()) return;
    const auto* target = codex_menu::SelectedTask(s_ui.menu_state);
    if (target == nullptr || target->host_id.empty() || target->thread_id.empty() ||
        s_ui.menu_state.stream_id.empty()) {
        SetStatus(true, "请先选择已同步任务");
        return;
    }
    const std::string request_id = "esp32-realtime-" + std::to_string(s_ui.menu_boot_nonce) +
        "-" + std::to_string(++s_ui.request_counter);
    const std::string request = TargetedRequest("realtime_start", request_id);
    if (request.empty() || !CodexWsClient::GetInstance().SendTextMessage(request)) {
        SetStatus(false, "实时语音连接请求失败");
        return;
    }
    s_ui.realtime_request_id = request_id;
    s_ui.realtime_host_id = target->host_id;
    s_ui.realtime_thread_id = target->thread_id;
    s_ui.realtime_stream_id = s_ui.menu_state.stream_id;
    s_ui.realtime_generation = 0;
    s_ui.realtime_last_sequence = 0;
    s_ui.realtime_muted = false;
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        s_ui.realtime_volume_percent = codec->output_volume();
        if (s_ui.realtime_volume) {
            std::string label = "音量 " + std::to_string(s_ui.realtime_volume_percent) + "%";
            lv_label_set_text(lv_obj_get_child(s_ui.realtime_volume, 0), label.c_str());
        }
    }
    SetRealtimeVisual(RealtimeVisual::Connecting, "连接中");
    ShowRealtimePage(true);
    SetStatus(true, "实时语音连接中…");
    HideConfig();
}

void OnRealtimeMute(lv_event_t*) {
    if (s_ui.realtime_request_id.empty()) return;
    const bool muted = !s_ui.realtime_muted;
    const std::string message = "{\"type\":\"realtime_mute\",\"requestId\":\"" +
        s_ui.realtime_request_id + "\",\"muted\":" + (muted ? "true}" : "false}");
    if (CodexWsClient::GetInstance().SendTextMessage(message, pdMS_TO_TICKS(200))) {
        // The visual state changes only when the PC confirms it in realtime_status.
        if (s_ui.realtime_mute) lv_label_set_text(lv_obj_get_child(s_ui.realtime_mute, 0), "静音中…");
    }
}

void OnRealtimeVolume(lv_event_t*) {
    s_ui.realtime_volume_percent = s_ui.realtime_volume_percent >= 100 ? 40 : s_ui.realtime_volume_percent + 10;
    if (auto* codec = Board::GetInstance().GetAudioCodec()) codec->SetOutputVolume(s_ui.realtime_volume_percent);
    if (s_ui.realtime_volume != nullptr) {
        std::string label = "音量 " + std::to_string(s_ui.realtime_volume_percent) + "%";
        lv_label_set_text(lv_obj_get_child(s_ui.realtime_volume, 0), label.c_str());
    }
}

void OnRealtimeEnd(lv_event_t*) { EndRealtimeSession(true); }

bool ShouldCaptureDeviceAudio(const cJSON* mode, const cJSON* audio_source,
                              const cJSON* accepts_audio) {
    if (s_ui.voice_mode == VoiceMode::Api) return true;
    return cJSON_IsString(mode) &&
           std::strcmp(mode->valuestring, "virtual_micro") == 0 &&
           cJSON_IsString(audio_source) &&
           std::strcmp(audio_source->valuestring, "esp32") == 0 &&
           cJSON_IsTrue(accepts_audio);
}

void SetTaskActive(bool active) {
    if (s_ui.task_active == active) return;
    s_ui.task_active = active;
    if (!active) {
        s_ui.stop_pending = false;
        ResetStopHoldProgress();
    }
    UpdateActionButton();
    UpdateStatusRing();
}

std::string TrimMarkdownLine(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    const auto last = line.find_last_not_of(" \t");
    return line.substr(first, last - first + 1);
}

std::string NormalizeInlineMarkdown(const std::string& source) {
    std::string output;
    output.reserve(source.size());
    for (size_t i = 0; i < source.size();) {
        if (source[i] == '\\' && i + 1 < source.size()) {
            output.push_back(source[i + 1]);
            i += 2;
            continue;
        }

        const bool image = source.compare(i, 2, "![") == 0;
        if (image || source[i] == '[') {
            const size_t label_start = i + (image ? 2 : 1);
            const size_t label_end = source.find("](", label_start);
            const size_t url_end = label_end == std::string::npos
                                       ? std::string::npos
                                       : source.find(')', label_end + 2);
            if (label_end != std::string::npos && url_end != std::string::npos) {
                output.append(source, label_start, label_end - label_start);
                if (!image) {
                    output.append(" (");
                    output.append(source, label_end + 2, url_end - label_end - 2);
                    output.push_back(')');
                }
                i = url_end + 1;
                continue;
            }
        }

        if (source.compare(i, 2, "**") == 0 ||
            source.compare(i, 2, "__") == 0) {
            i += 2;
            continue;
        }
        if (source[i] == '`') {
            ++i;
            continue;
        }
        if ((source[i] == '*' || source[i] == '_') &&
            source.find(source[i], i + 1) != std::string::npos) {
            ++i;
            continue;
        }
        output.push_back(source[i++]);
    }
    return output;
}

bool IsMarkdownRule(const std::string& line) {
    if (line.size() < 3) return false;
    const char marker = line.front();
    if (marker != '-' && marker != '*' && marker != '_') return false;
    for (char value : line) {
        if (value != marker && value != ' ') return false;
    }
    return true;
}

bool StripOrderedListPrefix(std::string* line) {
    if (line == nullptr || line->empty() || !std::isdigit((*line)[0])) return false;
    size_t index = 0;
    while (index < line->size() && std::isdigit((*line)[index])) ++index;
    if (index + 1 >= line->size() || (*line)[index] != '.' ||
        (*line)[index + 1] != ' ') {
        return false;
    }
    *line = line->substr(index + 2);
    return true;
}

void AddMarkdownLabel(lv_obj_t* parent, const std::string& text,
                      const lv_font_t* font, uint32_t color) {
    if (text.empty()) return;
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
}

void AddMarkdownRule(lv_obj_t* parent, uint32_t color) {
    lv_obj_t* rule = lv_obj_create(parent);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule, LV_OPA_30, LV_PART_MAIN);
}

void AddMarkdownCodeBlock(lv_obj_t* parent, const std::string& code,
                          uint32_t text_color, uint32_t background_color) {
    if (code.empty()) return;
    lv_obj_t* block = lv_obj_create(parent);
    lv_obj_set_width(block, LV_PCT(100));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(block, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(block, metrics::kRadiusControl, LV_PART_MAIN);
    lv_obj_set_style_border_width(block, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(block, lv_color_hex(background_color), LV_PART_MAIN);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    AddMarkdownLabel(block, code, fonts::Small(), text_color);
}

void RenderMarkdown(lv_obj_t* bubble, const char* markdown, bool user,
                    const ThemeColors& colors) {
    std::istringstream stream(markdown != nullptr ? markdown : "");
    std::string line;
    std::string code;
    bool in_code_block = false;
    const uint32_t text_color = user ? colors.accent_ink : colors.text;

    while (std::getline(stream, line)) {
        std::string trimmed = TrimMarkdownLine(line);
        if (trimmed.rfind("```", 0) == 0 || trimmed.rfind("~~~", 0) == 0) {
            if (in_code_block) {
                AddMarkdownCodeBlock(bubble, code, text_color,
                                     user ? colors.background : colors.surface);
                code.clear();
            }
            in_code_block = !in_code_block;
            continue;
        }
        if (in_code_block) {
            if (!code.empty()) code.push_back('\n');
            code += line;
            continue;
        }
        if (trimmed.empty()) continue;
        if (IsMarkdownRule(trimmed)) {
            AddMarkdownRule(bubble, text_color);
            continue;
        }

        const lv_font_t* font = fonts::Medium();
        std::string prefix;
        size_t heading_marks = 0;
        while (heading_marks < trimmed.size() && trimmed[heading_marks] == '#') {
            ++heading_marks;
        }
        if (heading_marks > 0 && heading_marks <= 3 &&
            heading_marks < trimmed.size() && trimmed[heading_marks] == ' ') {
            trimmed = trimmed.substr(heading_marks + 1);
            font = fonts::MediumBold();
        } else if (trimmed.rfind("> ", 0) == 0) {
            trimmed = trimmed.substr(2);
            prefix = "| ";
        } else if (trimmed.rfind("- ", 0) == 0 ||
                   trimmed.rfind("* ", 0) == 0 ||
                   trimmed.rfind("+ ", 0) == 0) {
            trimmed = trimmed.substr(2);
            prefix = "- ";
        } else {
            std::string ordered = trimmed;
            if (StripOrderedListPrefix(&ordered)) {
                const size_t marker_end = trimmed.find(". ");
                prefix = trimmed.substr(0, marker_end + 2);
                trimmed = ordered;
            }
        }

        if ((trimmed.size() >= 4 && trimmed.rfind("**", 0) == 0 &&
             trimmed.compare(trimmed.size() - 2, 2, "**") == 0) ||
            (trimmed.size() >= 4 && trimmed.rfind("__", 0) == 0 &&
             trimmed.compare(trimmed.size() - 2, 2, "__") == 0)) {
            font = fonts::MediumBold();
        }
        AddMarkdownLabel(bubble, prefix + NormalizeInlineMarkdown(trimmed), font,
                         text_color);
    }
    if (in_code_block) {
        AddMarkdownCodeBlock(bubble, code, text_color,
                             user ? colors.background : colors.surface);
    }
}

void SyncConversation() {
    const auto& state = s_ui.menu_state;
    if (s_ui.pending_menu_request.empty()) s_task_entry.Remember(state);
    const auto* target = codex_menu::SelectedTask(state);
    const bool new_task_draft = !target && !state.draft_request_id.empty();
    const std::string key = target ? target->host_id + ":" + target->thread_id :
                            new_task_draft ? "draft:" + state.draft_request_id : "";
    const bool changed_task = !s_ui.conversation_initialized || key != s_ui.displayed_task;
    if (s_ui.voice_pressed && s_ui.voice_target != CurrentVoiceTarget()) FinishVoiceCapture(true);
    if (s_ui.stop_pressed && s_ui.stop_target != CurrentVoiceTarget()) ResetStopHoldProgress();
    if (changed_task) {
        s_ui.stop_pending = false;
        ResetStopHoldProgress();
    }
    SetTaskActive(state.connected && target && codex_menu::IsBusy(*target));
    if (!changed_task && s_ui.config_overlay && !lv_obj_has_flag(s_ui.config_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    const bool draft_blocked = state.draft_status == "preparing" || state.draft_binding;
    const bool retryable_draft_error = state.draft_status == "error" && state.draft_submitted;
    if (s_ui.conversation_ui) s_ui.conversation_ui->Update(
        state.conversation, state.connected, new_task_draft, draft_blocked,
        state.draft_status == "error" ? state.draft_settings_error : std::string{},
        retryable_draft_error ? [] { RefreshDraftSettings(false); } : std::function<void()>{},
        state.draft_status);
    s_ui.displayed_task = key;
    s_ui.conversation_initialized = true;
}

std::string TextareaText(lv_obj_t* textarea) {
    if (textarea == nullptr) return "";
    const char* text = lv_textarea_get_text(textarea);
    return text != nullptr ? text : "";
}

bool HasNewConnectionInfo() {
    if (TextareaText(s_ui.token) != s_ui.connected_token) return true;
    if (s_ui.remote_mode != s_ui.connected_remote_mode) return true;
    return s_ui.remote_mode &&
           TextareaText(s_ui.remote_ip) != s_ui.connected_remote_ip;
}

void UpdateConnectionAction() {
    if (s_ui.connection_action.label == nullptr) return;
    const bool unchanged_connection =
        CodexWsClient::GetInstance().IsConnected() && !HasNewConnectionInfo();
    lv_label_set_text(s_ui.connection_action.label,
                      unchanged_connection ? "已连接" : "连接");
}

void CaptureConnectedConfig() {
    auto& client = CodexWsClient::GetInstance();
    std::string token;
    if (client.LoadToken(token)) s_ui.connected_token = token;
    else s_ui.connected_token.clear();
    s_ui.connected_remote_mode = s_ui.remote_mode;
    s_ui.connected_remote_ip = s_ui.remote_mode
                                   ? TextareaText(s_ui.remote_ip)
                                   : "";
    UpdateConnectionAction();
}

void OnConnectionInfoChanged(lv_event_t*) {
    UpdateConnectionAction();
}

void HideConfig() {
    Keyboard::Get().Hide();
    if (s_ui.config_overlay != nullptr) {
        lv_obj_add_flag(s_ui.config_overlay, LV_OBJ_FLAG_HIDDEN);
        SyncConversation();
    }
}

void ShowConfig() {
    if (s_ui.config_overlay == nullptr) return;
    lv_obj_remove_flag(s_ui.config_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.config_overlay);
    codex_status_ring::Raise(s_ui.ring_frame);
    if (s_ui.discovery_name != nullptr) {
        lv_label_set_text(
            s_ui.discovery_name,
            s_ui.discovered_name.empty() ? "正在自动发现 PC 服务..."
                                         : s_ui.discovered_name.c_str());
    }
    RefreshCodexMenu();
    if (!s_ui.remote_mode && !CodexWsClient::GetInstance().IsConnected()) CodexWsClient::GetInstance().StartDiscovery(8000);
    RefreshDraftSettings();
}

void HandleMessage(const std::string& message) {
    cJSON* root = cJSON_Parse(message.c_str());
    if (root == nullptr) return;
    const cJSON* type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    if (std::strcmp(type, "bridge_status") == 0) {
        const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
        const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
        const cJSON* codex_connected =
            cJSON_GetObjectItemCaseSensitive(root, "codexConnected");
        if (cJSON_IsNumber(version) && cJSON_IsString(state) &&
            cJSON_IsBool(codex_connected)) {
            s_ui.bridge_connection.ApplyBridgeStatus(
                version->valueint, state->valuestring,
                cJSON_IsTrue(codex_connected), lv_tick_get());
            RefreshBridgeUi(lv_tick_get());
        }
        cJSON_Delete(root);
        return;
    }
    // Only a current bridge_status may make task data usable.  In particular,
    // a delayed codex_state must not revive controls after a heartbeat timeout.
    if (!CanUseCodex()) {
        cJSON_Delete(root);
        return;
    }
    if (s_ui.conversation_ui && s_ui.conversation_ui->HandleMessage(message)) {
        cJSON_Delete(root);
        return;
    }
    if (std::strcmp(type, "chat") == 0 || std::strcmp(type, "status") == 0 ||
        std::strcmp(type, "stop") == 0 || std::strcmp(type, "task_complete") == 0 ||
        std::strcmp(type, "approval_request") == 0) {
        const cJSON* host = cJSON_GetObjectItemCaseSensitive(root, "host_id");
        const cJSON* thread = cJSON_GetObjectItemCaseSensitive(root, "thread_id");
        if (!cJSON_IsString(host) || !cJSON_IsString(thread) ||
            !codex_menu::MatchesSelectedTask(s_ui.menu_state, host->valuestring, thread->valuestring)) {
            cJSON_Delete(root); return;
        }
    }

    if (std::strcmp(type, "codex_state") == 0) {
        codex_menu::State incoming;
        const bool optimistic_new_task = s_ui.pending_new_task &&
            s_ui.menu_state.draft_status == "preparing" &&
            !s_ui.pending_menu_request.empty();
        const bool stale_before_new_task = optimistic_new_task &&
            codex_menu::ParseStateJson(message, &incoming) &&
            incoming.connected && incoming.stream_id == s_ui.menu_state.stream_id &&
            incoming.draft_request_id != s_ui.menu_state.draft_request_id;
        if (!stale_before_new_task && codex_menu::ApplyStateJson(message, &s_ui.menu_state)) {
            IdlePower::Get().SetMicroDisplay(ParseMicroDisplay(root));
            TrySelectEntryTask();
            SyncConversation();
            RefreshCodexMenu();
            UpdateStatusRing();
        }
    } else if (std::strcmp(type, "codex_action_result") == 0) {
        std::string error;
        const cJSON* request = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        if (!cJSON_IsString(request) || s_ui.pending_menu_request != request->valuestring) {
            cJSON_Delete(root);
            return;
        }
        const cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
        const bool new_task = cJSON_IsString(action) &&
                              std::strcmp(action->valuestring, "new_task") == 0;
        if (codex_menu::ApplyActionResultJson(message, &s_ui.menu_state, &error)) {
            s_ui.pending_menu_request.clear();
            if (new_task) s_ui.pending_new_task = false;
            if (s_ui.menu_pending_timer) { lv_timer_delete(s_ui.menu_pending_timer); s_ui.menu_pending_timer = nullptr; }
            if (cJSON_IsString(action) && std::strcmp(action->valuestring, "select_task") == 0) HideConfig();
            SyncConversation();
            RefreshCodexMenu();
            UpdateStatusRing();
            if (new_task) SetStatus(true, "新任务已就绪");
        } else if (!error.empty()) {
            SetStatus(true, error.c_str());
            s_ui.pending_menu_request.clear();
            if (new_task) s_ui.pending_new_task = false;
            if (s_ui.menu_pending_timer) { lv_timer_delete(s_ui.menu_pending_timer); s_ui.menu_pending_timer = nullptr; }
            SyncConversation();
            RefreshCodexMenu();
        }
    } else if (std::strcmp(type, "status") == 0) {
        const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(state)) {
            if (IsWorkingState(state->valuestring)) SetTaskActive(true);
            if (IsIdleState(state->valuestring)) SetTaskActive(false);
            UpdateStatusRing();
        }
    } else if (std::strcmp(type, "realtime_status") == 0) {
        if (!IsCurrentRealtime(root)) {
            cJSON_Delete(root);
            return;
        }
        const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
        const cJSON* accepts_audio = cJSON_GetObjectItemCaseSensitive(root, "acceptsAudio");
        const cJSON* detail = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (!cJSON_IsString(state)) {
            cJSON_Delete(root);
            return;
        }
        const char* value = state->valuestring;
        const bool input_phase = std::strcmp(value, "listening") == 0 ||
                                 std::strcmp(value, "thinking") == 0;
        if (input_phase && cJSON_IsTrue(accepts_audio) && !s_ui.realtime_capture_started) {
            s_ui.realtime_capture_started = true;
            Application::GetInstance().StartCodexRealtimeCapture(s_ui.realtime_request_id);
        } else if ((!input_phase || !cJSON_IsTrue(accepts_audio)) && s_ui.realtime_capture_started) {
            s_ui.realtime_capture_started = false;
            Application::GetInstance().StopCodexRealtimeCapture();
        }
        if (std::strcmp(value, "listening") == 0) {
            // Sending realtime_start only asks the PC to prepare. Capture
            // starts solely after its matching, identity-bound ready status.
            s_ui.realtime_muted = false;
            SetRealtimeVisual(RealtimeVisual::Listening, "聆听中");
            if (s_ui.realtime_mute) lv_label_set_text(lv_obj_get_child(s_ui.realtime_mute, 0), "静音");
            SetStatus(true, cJSON_IsTrue(accepts_audio) ? "实时语音正在聆听" : "实时语音等待麦克风");
        } else if (std::strcmp(value, "connecting") == 0) {
            SetRealtimeVisual(RealtimeVisual::Connecting, "连接中");
            SetStatus(true, "实时语音连接中…");
        } else if (std::strcmp(value, "thinking") == 0 || std::strcmp(value, "speaking") == 0 ||
                   std::strcmp(value, "muted") == 0) {
            if (std::strcmp(value, "speaking") == 0) SetRealtimeVisual(RealtimeVisual::Speaking, "说话中");
            else if (std::strcmp(value, "muted") == 0) {
                s_ui.realtime_muted = true;
                SetRealtimeVisual(RealtimeVisual::Muted, "已静音");
                if (s_ui.realtime_mute) lv_label_set_text(lv_obj_get_child(s_ui.realtime_mute, 0), "取消静音");
            } else SetRealtimeVisual(RealtimeVisual::Thinking, "思考中");
            SetStatus(true, std::strcmp(value, "speaking") == 0 ? "实时语音正在说话" :
                            std::strcmp(value, "muted") == 0 ? "实时语音已静音" : "实时语音思考中");
        } else if (std::strcmp(value, "ended") == 0 || std::strcmp(value, "error") == 0 ||
                   std::strcmp(value, "disconnected") == 0) {
            const bool failed = std::strcmp(value, "error") == 0 || std::strcmp(value, "disconnected") == 0;
            EndRealtimeSession(false);
            SetRealtimeVisual(failed ? RealtimeVisual::Error : RealtimeVisual::Connecting,
                              cJSON_IsString(detail) ? detail->valuestring : "已结束");
            SetStatus(!failed, cJSON_IsString(detail) ? detail->valuestring :
                      (failed ? "实时语音已断开" : "实时语音已结束"));
            // The PC-provided setup failure must remain readable; do not
            // reduce it to an icon after the standalone call surface closes.
            if (failed) ShowConfig();
        }
    } else if (std::strcmp(type, "realtime_audio_clear") == 0) {
        if (IsCurrentRealtime(root)) {
            const cJSON* generation = cJSON_GetObjectItemCaseSensitive(root, "generation");
            if (cJSON_IsNumber(generation) && generation->valuedouble > s_ui.realtime_generation) {
                s_ui.realtime_generation = static_cast<uint32_t>(generation->valuedouble);
                s_ui.realtime_last_sequence = 0;
                Application::GetInstance().ClearCodexRealtimeAudio();
            }
        }
    } else if (std::strcmp(type, "realtime_audio") == 0) {
        if (IsCurrentRealtime(root)) {
            const cJSON* generation = cJSON_GetObjectItemCaseSensitive(root, "generation");
            const cJSON* sequence = cJSON_GetObjectItemCaseSensitive(root, "sequence");
            const cJSON* rate = cJSON_GetObjectItemCaseSensitive(root, "sampleRate");
            const cJSON* duration = cJSON_GetObjectItemCaseSensitive(root, "frameDuration");
            const cJSON* codec = cJSON_GetObjectItemCaseSensitive(root, "codec");
            const cJSON* encoded = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsNumber(generation) && cJSON_IsNumber(sequence) && cJSON_IsNumber(rate) &&
                cJSON_IsNumber(duration) && cJSON_IsString(codec) && cJSON_IsString(encoded) &&
                std::strcmp(codec->valuestring, "opus") == 0 && rate->valueint == 16000 &&
                duration->valueint == 20 && std::strlen(encoded->valuestring) <= 5464) {
                const uint32_t frame_generation = static_cast<uint32_t>(generation->valuedouble);
                const uint32_t frame_sequence = static_cast<uint32_t>(sequence->valuedouble);
                if (frame_generation == s_ui.realtime_generation && frame_sequence > s_ui.realtime_last_sequence) {
                    size_t decoded_size = 0;
                    const int sizing = mbedtls_base64_decode(nullptr, 0, &decoded_size,
                        reinterpret_cast<const unsigned char*>(encoded->valuestring),
                        std::strlen(encoded->valuestring));
                    if (sizing == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && decoded_size > 0 && decoded_size <= 4096) {
                        auto packet = std::make_unique<AudioStreamPacket>();
                        packet->payload.resize(decoded_size);
                        size_t written = 0;
                        if (mbedtls_base64_decode(packet->payload.data(), packet->payload.size(), &written,
                                reinterpret_cast<const unsigned char*>(encoded->valuestring),
                                std::strlen(encoded->valuestring)) == 0 && written == decoded_size) {
                            packet->sample_rate = 16000;
                            packet->frame_duration = 20;
                            if (Application::GetInstance().PushCodexRealtimeAudio(std::move(packet))) {
                                s_ui.realtime_last_sequence = frame_sequence;
                            }
                        }
                    }
                }
            }
        }
    } else if (std::strcmp(type, "turn_stop_result") == 0) {
        const cJSON* request = cJSON_GetObjectItemCaseSensitive(root, "requestId");
        if (cJSON_IsString(request) && !s_ui.stop_request_id.empty() &&
            s_ui.stop_request_id == request->valuestring && s_ui.stop_target == CurrentVoiceTarget()) {
            s_ui.stop_pending = false;
            s_ui.stop_request_id.clear();
            const bool success = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "success"));
            SetStatus(true, success ? "已请求停止" : "停止失败，请重试");
            UpdateActionButton();
        }
    } else if (std::strcmp(type, "voice_status") == 0) {
        const cJSON* request_id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
        if (!cJSON_IsString(request_id) || s_ui.voice_request_id.empty() ||
            s_ui.voice_request_id != request_id->valuestring) {
            cJSON_Delete(root);
            return;
        }
        const cJSON* mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        if (cJSON_IsString(mode)) {
            if (std::strcmp(mode->valuestring, "api") == 0) {
                s_ui.voice_mode = VoiceMode::Api;
            } else if (std::strcmp(mode->valuestring, "virtual_micro") == 0) {
                s_ui.voice_mode = VoiceMode::Micro;
            }
        }
        const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(state)) {
            if (std::strcmp(state->valuestring, "preparing") == 0) {
                if (s_ui.voice_pressed && s_ui.voice_stage == VoiceStage::Preparing) {
                    SetVoiceStage(VoiceStage::Preparing);
                }
            } else if (std::strcmp(state->valuestring, "recording") == 0) {
                if (s_ui.voice_pressed) {
                    const cJSON* audio_source =
                        cJSON_GetObjectItemCaseSensitive(root, "audioSource");
                    const cJSON* accepts_audio =
                        cJSON_GetObjectItemCaseSensitive(root, "acceptsAudio");
                    if (ShouldCaptureDeviceAudio(mode, audio_source, accepts_audio) &&
                        !s_ui.voice_capture_started) {
                        s_ui.voice_capture_started = true;
                        Application::GetInstance().StartCodexVoiceCapture();
                    }
                    SetVoiceStage(VoiceStage::Recording);
                }
            } else if (std::strcmp(state->valuestring, "stopped") == 0) {
                StopDeviceVoiceCapture();
                s_ui.voice_pressed = false;
                s_ui.voice_cancel_armed = false;
                s_ui.voice_cancel_pending = false;
                s_ui.voice_request_id.clear();
                SetVoiceStage(VoiceStage::Idle);
                if (s_ui.voice_mode == VoiceMode::Micro) {
                    const bool requested = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "submissionRequested"));
                    const bool confirmed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "submissionConfirmed"));
                    const cJSON* submission = cJSON_GetObjectItemCaseSensitive(root, "submission");
                    const bool steer = cJSON_IsString(submission) && std::strcmp(submission->valuestring, "steer") == 0;
                    const bool queued = cJSON_IsString(submission) && std::strcmp(submission->valuestring, "queue") == 0;
                    const bool cancelled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "cancelled"));
                    SetStatus(true, requested ? (confirmed ? (queued ? "已排队" : steer ? "已引导" : "已发送") :
                        (steer ? "已请求引导" : "已请求发送")) : cancelled ? "已取消听写" : "语音未发送，请重试");
                }
            } else if (std::strcmp(state->valuestring, "recognizing") == 0) {
                StopDeviceVoiceCapture();
                s_ui.voice_pressed = false;
                SetVoiceStage(VoiceStage::Recognizing);
                SetStatus(true, "转录中...");
            } else if (std::strcmp(state->valuestring, "submitting") == 0) {
                SetVoiceStage(VoiceStage::Submitting);
            } else if (std::strcmp(state->valuestring, "submitted") == 0) {
                s_ui.voice_request_id.clear();
                SetVoiceStage(VoiceStage::Idle);
                if (s_ui.voice_mode == VoiceMode::Api ||
                    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "submissionConfirmed"))) {
                    SetTaskActive(true);
                }
            } else if (std::strcmp(state->valuestring, "error") == 0) {
                StopDeviceVoiceCapture();
                s_ui.voice_pressed = false;
                s_ui.voice_cancel_armed = false;
                s_ui.voice_cancel_pending = false;
                s_ui.voice_request_id.clear();
                CancelVoiceLabelTimer();
                const cJSON* detail = cJSON_GetObjectItemCaseSensitive(root, "message");
                SetStatus(true, cJSON_IsString(detail) ? detail->valuestring : "发送失败，请重试");
                {
                    SetVoiceStage(VoiceStage::Error);
                    s_ui.voice_label_timer = lv_timer_create(
                        [](lv_timer_t* timer) {
                            s_ui.voice_label_timer = nullptr;
                            SetVoiceStage(VoiceStage::Idle);
                            lv_timer_delete(timer);
                        },
                        1600, nullptr);
                }
            }
        }
    }
    cJSON_Delete(root);
    RefreshBridgeUi(lv_tick_get());
}

void PostMessage(const std::string& message) {
    lv_obj_t* root = s_ui.root.load();
    UiDispatcher::Post([root, message]() {
        if (root != nullptr && s_ui.root.load() == root) HandleMessage(message);
    });
}

void PostStatus(bool connected) {
    lv_obj_t* root = s_ui.root.load();
    UiDispatcher::Post([root, connected]() {
        if (root == nullptr || s_ui.root.load() != root) return;
        s_ui.bridge_connection.SetTransportConnected(connected, lv_tick_get());
        RefreshBridgeUi(lv_tick_get());
        if (connected) {
            CaptureConnectedConfig();
            HideConfig();
        }
        UpdateConnectionAction();
    });
}

void PostDiscovery(const std::string& name, const std::string& ip, int port) {
    lv_obj_t* root = s_ui.root.load();
    UiDispatcher::Post([root, name, ip, port]() {
        if (root == nullptr || s_ui.root.load() != root) return;
        s_ui.discovered_name = name;
        s_ui.discovered_ip = ip;
        s_ui.discovered_port = port;
        if (s_ui.discovery_name != nullptr) {
            lv_label_set_text(s_ui.discovery_name, name.c_str());
        }
        if (!CodexWsClient::GetInstance().HasToken()) ShowConfig();
    });
}

void CancelVoiceLabelTimer() {
    if (s_ui.voice_label_timer == nullptr) return;
    lv_timer_delete(s_ui.voice_label_timer);
    s_ui.voice_label_timer = nullptr;
}

void FinishVoiceCapture(bool cancelled) {
    if (!s_ui.voice_pressed) return;
    s_ui.voice_pressed = false;
    s_ui.voice_cancel_armed = false;
    s_ui.voice_cancel_pending = cancelled;
    SetVoiceStage(cancelled ? VoiceStage::Finishing : s_ui.voice_mode == VoiceMode::Api
                      ? VoiceStage::AwaitingRecognition
                      : VoiceStage::Finishing);
    const std::string request_id = s_ui.voice_request_id;
    auto send_voice_end = [request_id, cancelled]() {
        auto& ws = CodexWsClient::GetInstance();
        if (!ws.SendTextMessage("{\"type\":\"voice_end\",\"requestId\":\"" +
                                request_id + (cancelled ? "\",\"cancelled\":true}" : "\"}"))) {
            // Closing the transport also releases PTT in the PC bridge.
            ws.Reconnect();
        }
    };
    if (cancelled) {
        // Do not wait for queued audio to drain before asking the PC to discard.
        StopDeviceVoiceCapture();
        send_voice_end();
    } else {
        StopDeviceVoiceCapture(std::move(send_voice_end));
    }
}

void InvalidateCodexSession() {
    // A bridge/session replacement must never leave its microphone or queued
    // playback attached to a task that is no longer selected.
    EndRealtimeSession(false);
    if (s_ui.pending_menu_request.empty()) s_task_entry.Remember(s_ui.menu_state);
    s_task_entry.Begin();
    s_ui.pending_menu_request.clear();
    s_ui.pending_new_task = false;
    if (s_ui.menu_pending_timer != nullptr) {
        lv_timer_delete(s_ui.menu_pending_timer);
        s_ui.menu_pending_timer = nullptr;
    }
    CancelVoiceLabelTimer();
    const std::string cancelled_voice_request = s_ui.voice_request_id;
    StopDeviceVoiceCapture([cancelled_voice_request]() {
        if (cancelled_voice_request.empty() ||
            !CodexWsClient::GetInstance().IsConnected()) return;
        CodexWsClient::GetInstance().SendTextMessage(
            "{\"type\":\"voice_end\",\"requestId\":\"" +
                cancelled_voice_request + "\",\"cancelled\":true}",
            pdMS_TO_TICKS(200));
    });
    s_ui.voice_pressed = false;
    s_ui.voice_cancel_armed = false;
    s_ui.voice_cancel_pending = false;
    s_ui.voice_request_id.clear();
    s_ui.voice_target.clear();
    s_ui.voice_mode = VoiceMode::Micro;
    SetVoiceStage(VoiceStage::Idle);
    s_ui.stop_pending = false;
    s_ui.stop_request_id.clear();
    s_ui.stop_target.clear();
    ResetStopHoldProgress();
    SetTaskActive(false);
    // CodexConversationUi keeps the already rendered transcript.  Clearing
    // the source snapshot prevents stale task/menu controls from being reused.
    s_ui.menu_state = {};
    if (s_ui.conversation_ui) s_ui.conversation_ui->Disconnected();
    RefreshCodexMenu(true);
}

void RefreshBridgeUi(uint32_t now_ms) {
    const auto status = s_ui.bridge_connection.Current(now_ms);
    const bool usable = status == codex_bridge::Status::Ready;
    if (!usable) IdlePower::Get().SetMicroDisplay({});
    if (s_ui.bridge_was_usable && !usable) InvalidateCodexSession();
    s_ui.bridge_was_usable = usable;

    switch (status) {
        case codex_bridge::Status::TransportDisconnected:
            SetStatus(false, "PC 桥接已断开 · 正在重连");
            break;
        case codex_bridge::Status::Synchronizing:
            SetStatus(true, "PC 桥接已连接 · 正在同步");
            break;
        case codex_bridge::Status::Stopping:
            SetStatus(false, "PC 桥接正在停止");
            break;
        case codex_bridge::Status::Unresponsive:
            SetStatus(false, "PC 桥接无响应 · 等待恢复");
            break;
        case codex_bridge::Status::CodexDisconnected:
            SetStatus(false, "PC 桥接已连接 · Codex 已断开");
            break;
        case codex_bridge::Status::Ready:
            SetStatus(true, "PC 桥接 / Codex 已连接");
            break;
    }

    if (s_ui.bridge_connection.ConsumeSyncRequest(now_ms)) {
        // bridge_status is the liveness source.  The bridge replies with a
        // heartbeat followed by a fresh snapshot, including after recovery.
        if (!CodexWsClient::GetInstance().SendTextMessage(
                "{\"type\":\"codex_sync\"}", pdMS_TO_TICKS(200))) {
            // Keep the bridge liveness state unchanged, but retry the fresh
            // snapshot request on the next bounded UI refresh.
            s_ui.bridge_connection.ResetSyncRequest();
        }
    }
    UpdateActionButton();
    UpdateStatusRing();
}

void StartVoiceCapture() {
    auto& client = CodexWsClient::GetInstance();
    if (!CanUseCodex()) {
        ShowConfig();
        return;
    }
    const bool draft_ready = s_ui.menu_state.draft_request_id.empty() ||
        (s_ui.menu_state.draft_status == "editing" && !s_ui.menu_state.draft_settings_loading);
    if (s_ui.voice_stage != VoiceStage::Idle || s_ui.stop_pending || s_ui.stop_pressed ||
        !s_ui.pending_menu_request.empty() || !draft_ready) {
        if (!draft_ready) SetStatus(true, "新任务准备中，请稍后");
        return;
    }
    CancelVoiceLabelTimer();
    s_ui.voice_request_id = "esp32-voice-" +
                            std::to_string(s_ui.menu_boot_nonce) + "-" +
                            std::to_string(++s_ui.request_counter);
    const auto request = TargetedRequest("voice_start", s_ui.voice_request_id);
    if (request.empty() || !client.SendTextMessage(request)) {
        s_ui.voice_request_id.clear();
        SetStatus(true, "请等待任务同步后再说话");
        return;
    }
    s_ui.voice_target = CurrentVoiceTarget();
    s_ui.voice_pressed = true;
    s_ui.voice_cancel_armed = false;
    s_ui.voice_cancel_pending = false;
    s_ui.voice_mode = VoiceMode::Micro;
    SetStatus(true);
    SetVoiceStage(VoiceStage::Preparing);
}

void ResetStopHoldProgress() {
    s_ui.stop_pressed = false;
    if (s_ui.stop_hold_timer != nullptr) {
        lv_timer_delete(s_ui.stop_hold_timer);
        s_ui.stop_hold_timer = nullptr;
    }
    if (s_ui.stop_hold_progress != nullptr) {
        lv_obj_set_width(s_ui.stop_hold_progress, 0);
    }
    if (s_ui.stop_label != nullptr) lv_label_set_text(s_ui.stop_label, s_ui.stop_pending ? "正在停止…" : "停止");
}

void SendStopRequest() {
    if (!CanUseCodex() || !s_ui.task_active || s_ui.stop_pending ||
        s_ui.stop_target != CurrentVoiceTarget()) return;
    const std::string id = "esp32-stop-" +
                           std::to_string(s_ui.menu_boot_nonce) + "-" +
                           std::to_string(++s_ui.request_counter);
    const auto request = TargetedRequest("turn_stop", id);
    if (!request.empty() && CodexWsClient::GetInstance().SendTextMessage(request)) {
        s_ui.stop_request_id = id;
        s_ui.stop_pending = true;
        UpdateActionButton();
    }
}

void OnStopHoldTimer(lv_timer_t*) {
    if (!s_ui.stop_pressed || !s_ui.task_active || s_ui.stop_pending ||
        s_ui.voice_stage != VoiceStage::Idle || !CanUseCodex() ||
        s_ui.stop_target != CurrentVoiceTarget()) {
        ResetStopHoldProgress();
        return;
    }
    const uint32_t elapsed = lv_tick_elaps(s_ui.stop_hold_started_at);
    const uint32_t progress = std::min<uint32_t>(
        100, elapsed * 100 / kStopHoldDurationMs);
    if (s_ui.stop_hold_progress != nullptr) {
        lv_obj_set_width(s_ui.stop_hold_progress, LV_PCT(static_cast<int32_t>(progress)));
    }
    if (elapsed < kStopHoldDurationMs) return;

    if (s_ui.stop_hold_timer != nullptr) {
        lv_timer_delete(s_ui.stop_hold_timer);
        s_ui.stop_hold_timer = nullptr;
    }
    if (!s_ui.stop_triggered) { s_ui.stop_triggered = true; SendStopRequest(); }
}

void StartStopHold() {
    if (s_ui.stop_hold_timer != nullptr || s_ui.stop_pending) return;
    s_ui.stop_pressed = true;
    s_ui.stop_triggered = false;
    s_ui.stop_target = CurrentVoiceTarget();
    s_ui.stop_hold_started_at = lv_tick_get();
    if (s_ui.stop_hold_progress != nullptr) {
        // Make the progress affordance visible on the initial press; the timer
        // then advances it linearly until the stop request is sent.
        lv_obj_set_width(s_ui.stop_hold_progress, 1);
    }
    s_ui.stop_hold_timer = lv_timer_create(
        OnStopHoldTimer, kStopHoldUpdateMs, nullptr);
    UpdateActionButton();
}

void OnVoicePressed(lv_event_t* event) {
    if (!CanUseCodex()) {
        ShowConfig();
        return;
    }
    if (s_ui.voice_stage != VoiceStage::Idle) {
        return;
    }
    // PTT follows the physical press. Device audio starts only after an API
    // response or an explicit virtual-micro ESP32-audio capability response.
    lv_indev_t* input = event ? lv_event_get_indev(event) : lv_indev_active();
    lv_point_t point{};
    if (input) lv_indev_get_point(input, &point);
    s_ui.voice_press_y = point.y;
    StartVoiceCapture();
}

void OnVoicePressing(lv_event_t* event) {
    if (!s_ui.voice_pressed) return;
    lv_indev_t* input = event ? lv_event_get_indev(event) : lv_indev_active();
    if (!input) return;
    lv_point_t point{};
    lv_indev_get_point(input, &point);
    const bool cancel = s_ui.voice_press_y - point.y >= 64;
    if (cancel != s_ui.voice_cancel_armed) {
        s_ui.voice_cancel_armed = cancel;
        UpdateActionButton();
    }
}

void OnVoiceReleased(lv_event_t* event) {
    OnVoicePressing(event);
    const bool lost = event && lv_event_get_code(event) == LV_EVENT_PRESS_LOST;
    FinishVoiceCapture(lost || s_ui.voice_cancel_armed);
    UpdateStatusRing();
}

void OnStopPressed(lv_event_t*) {
    if (s_ui.task_active && s_ui.voice_stage == VoiceStage::Idle &&
        CanUseCodex()) StartStopHold();
}

void OnStopReleased(lv_event_t*) { ResetStopHoldProgress(); }

void OnSaveToken(lv_event_t*) {
    if (s_ui.token == nullptr) return;
    auto& client = CodexWsClient::GetInstance();
    if (client.IsConnected() && !HasNewConnectionInfo()) {
        HideConfig();
        return;
    }
    std::string remote_uri;
    if (s_ui.remote_mode) {
        std::string host;
        int port = 0;
        if (!codex_remote::ParseEndpoint(TextareaText(s_ui.remote_ip), remote_uri, host, port)) {
            return;
        }
    }
    const char* token = lv_textarea_get_text(s_ui.token);
    if (token == nullptr || !client.SaveToken(token)) return;
    Keyboard::Get().Hide();
    if (s_ui.remote_mode) {
        {
            Settings settings("codex", true);
            if (settings.GetString("remote_url") != remote_uri) {
                settings.SetString("remote_url", remote_uri);
            }
        }
        client.Connect(remote_uri);
    } else {
        const std::string ip = !s_ui.discovered_ip.empty()
                                   ? s_ui.discovered_ip
                                   : client.GetCurrentIp();
        const int port = !s_ui.discovered_ip.empty()
                             ? s_ui.discovered_port
                             : client.GetCurrentPort();
        if (!ip.empty()) client.Connect(ip, port);
        else client.StartDiscovery(8000);
    }
}

void SetConnectionMode(bool remote) {
    s_ui.remote_mode = remote;
    CodexWsClient::GetInstance().SetDiscoveryEnabled(!remote);
    for (size_t i = 0; i < 2; ++i) {
        controls::SetSegmentButtonSelected(s_ui.connection_modes[i],
                                           i == static_cast<size_t>(remote));
        if (s_ui.connection_panels[i] == nullptr) continue;
        if (i == static_cast<size_t>(remote)) {
            lv_obj_remove_flag(s_ui.connection_panels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.connection_panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!remote) CodexWsClient::GetInstance().StartDiscovery(8000);
    UpdateConnectionAction();
}

void OnConnectionMode(lv_event_t* event) {
    SetConnectionMode(reinterpret_cast<uintptr_t>(
                          lv_event_get_user_data(event)) != 0);
}

void OnOpenConfig(lv_event_t*) { ShowConfig(); }

void AddMenuGlyph(lv_obj_t* button) {
    if (button == nullptr) return;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* line = lv_obj_create(button);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 28, 3);
        lv_obj_set_style_bg_color(
            line, lv_color_hex(Theme::Get().colors().muted), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(line, 2, LV_PART_MAIN);
        lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 12 + i * 8);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
}

void BuildConfigDialog(lv_obj_t* root) {
    codex_menu_ui::Callbacks callbacks{};
    callbacks.dismiss = [](lv_event_t* event) {
        if (lv_event_get_target_obj(event) == lv_event_get_current_target_obj(event)) HideConfig();
    };
    callbacks.tab = OnMenuTab;
    callbacks.task = OnTaskSlot;
    callbacks.new_task = OnMenuNewTask;
    callbacks.realtime = OnMenuRealtime;
    callbacks.model = OnModelDropdown;
    callbacks.effort = OnEffortReleased;
    callbacks.fast = OnFastChanged;
    callbacks.ring = OnRingChanged;
    callbacks.notification_enabled = [](lv_event_t* event) {
        const bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
        codex_notification::Get().SetEnabled(enabled);
        codex_menu_ui::SetNotificationSettings(s_ui.menu_ui, enabled);
    };
    callbacks.mode = OnConnectionMode;
    callbacks.connection_changed = OnConnectionInfoChanged;
    callbacks.connect = OnSaveToken;
    s_ui.menu_ui = codex_menu_ui::Build(root, callbacks);
    const auto notification_settings = codex_notification::Get().GetSettings();
    codex_menu_ui::SetNotificationSettings(s_ui.menu_ui, notification_settings.enabled);
    const auto& parts = s_ui.menu_ui;
    s_ui.config_overlay = parts.overlay;
    lv_obj_add_flag(s_ui.config_overlay, LV_OBJ_FLAG_HIDDEN);
    s_ui.discovery_name = parts.discovery_name;
    for (int i = 0; i < 2; ++i) {
        s_ui.connection_modes[i] = parts.modes[i];
        s_ui.connection_panels[i] = parts.connection_panels[i];
    }
    for (int i = 0; i < 3; ++i) {
        s_ui.menu_tabs[i] = parts.tabs[i];
        s_ui.menu_panels[i] = parts.panels[i];
    }
    for (int i = 0; i < 6; ++i) s_ui.task_cards[i] = parts.tasks[i].root;
    s_ui.connection_action = parts.connect;
    s_ui.remote_ip = parts.remote_ip;
    s_ui.token = parts.token;
    s_ui.new_task_button = parts.new_task;
    s_ui.realtime_button = parts.realtime;
    s_ui.model_summary = parts.model_context;
    s_ui.model_dropdown = parts.model_dropdown;
    s_ui.fast_switch = parts.fast_switch;
    std::string saved_token;
    if (CodexWsClient::GetInstance().LoadToken(saved_token)) {
        lv_textarea_set_text(s_ui.token, saved_token.c_str());
    }
    Keyboard::Get().Bind(s_ui.remote_ip, "服务器地址", LV_KEYBOARD_MODE_TEXT_LOWER);
    Keyboard::Get().Bind(s_ui.token, "Codex Token");
    Settings settings("codex", false);
    const std::string saved_remote_url = settings.GetString("remote_url");
    lv_textarea_set_text(s_ui.remote_ip, saved_remote_url.c_str());
    s_ui.ring_enabled = settings.GetBool("status_ring", true);
    if (s_ui.ring_enabled) lv_obj_add_state(parts.ring_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(parts.ring_switch, LV_STATE_CHECKED);
    CaptureConnectedConfig();
    SetMenuTab(MenuTab::Tasks);
    RefreshCodexMenu();
}

void SetMenuTab(MenuTab tab) {
    codex_menu_ui::SelectTab(s_ui.menu_ui, static_cast<int>(tab));
    RefreshCodexMenu();
}

bool MenuVoiceBusy() { return !s_ui.realtime_request_id.empty() || s_ui.voice_pressed || s_ui.voice_stage != VoiceStage::Idle; }

bool SendMenuAction(const char* action, int slot = -1, const std::string& model = {},
                    const std::string& effort = {}, int fast = -1) {
    if (!CanUseCodex() || !s_ui.pending_menu_request.empty()) return false;
    const std::string request_id = "esp32-menu-" + std::to_string(s_ui.menu_boot_nonce) + "-" + std::to_string(++s_ui.request_counter);
    const bool setting = std::strcmp(action, "set_model") == 0 || std::strcmp(action, "set_effort") == 0 || std::strcmp(action, "set_fast") == 0;
    const auto& target = slot >= 0 && slot < 6 ? s_ui.menu_state.slots[slot] :
        setting ? s_ui.menu_state.active_task : codex_menu::Slot{};
    const bool draft = slot < 0 && !codex_menu::SelectedTask(s_ui.menu_state) &&
        !s_ui.menu_state.draft_request_id.empty() &&
        (std::strcmp(action, "set_model") == 0 || std::strcmp(action, "set_effort") == 0 ||
         std::strcmp(action, "set_fast") == 0 || std::strcmp(action, "refresh_draft_settings") == 0);
    const std::string json = codex_menu::BuildActionJson(request_id, action, slot, model, effort, fast,
        target.thread_id, draft ? "local" : target.host_id,
        draft ? s_ui.menu_state.draft_request_id : "", draft || (setting && slot < 0) ? s_ui.menu_state.stream_id : "");
    if (json.empty() || !CodexWsClient::GetInstance().SendTextMessage(json)) return false;
    s_ui.pending_menu_request = request_id;
    // Explicit task selection/new-task actions also settle this page entry, so
    // a later state snapshot cannot override the user's choice.
    if (std::strcmp(action, "select_task") == 0 || std::strcmp(action, "new_task") == 0) s_task_entry.Complete();
    const bool new_task = std::strcmp(action, "new_task") == 0;
    if (new_task) {
        s_ui.pending_new_task = true;
        codex_menu::BeginDraft(&s_ui.menu_state, request_id);
        HideConfig();
        SyncConversation();
        SetStatus(true, "正在准备新任务");
    }
    s_ui.menu_pending_timer = lv_timer_create([](lv_timer_t* timer) {
        s_ui.menu_pending_timer = nullptr;
        const bool new_task = s_ui.pending_new_task;
        s_ui.pending_new_task = false;
        if (new_task) codex_menu::FailDraft(&s_ui.menu_state, "新任务准备超时，请重试");
        s_ui.pending_menu_request.clear();
        if (new_task) SyncConversation();
        RefreshCodexMenu();
        lv_timer_delete(timer);
    }, draft || new_task ? 25000 : 15000, nullptr);
    RefreshCodexMenu();
    return true;
}

void TrySelectEntryTask() {
    if (!CanUseCodex() || MenuVoiceBusy() ||
        s_ui.stop_pressed || s_ui.stop_pending || !s_ui.pending_menu_request.empty()) return;
    const auto* target = s_task_entry.Resolve(s_ui.menu_state);
    if (!target) return;  // Wait for the first connected snapshot with tasks.
    if (codex_menu::MatchesSelectedTask(s_ui.menu_state, target->host_id, target->thread_id)) {
        s_task_entry.Complete();
    } else if (s_ui.menu_state.can_select_task && target->slot >= 0 && target->slot < 6) {
        SendMenuAction("select_task", target->slot);
    }
}

void OnMenuTab(lv_event_t* event) {
    SetMenuTab(static_cast<MenuTab>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
    RefreshDraftSettings();
}

void RefreshDraftSettings(bool require_model_tab) {
    if ((!require_model_tab || s_ui.menu_ui.selected_tab == 1) && !MenuVoiceBusy() &&
        !s_ui.menu_state.draft_request_id.empty() && !codex_menu::SelectedTask(s_ui.menu_state) &&
        !s_ui.menu_state.draft_settings_loading &&
        s_ui.menu_state.draft_submitted && s_ui.menu_state.draft_status == "error") {
        SendMenuAction("refresh_draft_settings");
    }
}

void OnTaskSlot(lv_event_t* event) {
    const int slot = static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (!CanUseCodex() || slot < 0 || slot >= 6 || MenuVoiceBusy() || !s_ui.menu_state.can_select_task || !codex_menu::IsBound(s_ui.menu_state.slots[slot])) return;
    SendMenuAction("select_task", slot);
}

void OnMenuNewTask(lv_event_t*) { if (CanUseCodex() && !MenuVoiceBusy() && s_ui.menu_state.can_new_task) SendMenuAction("new_task"); }

void OnMenuRealtime(lv_event_t*) { StartRealtimeSession(); }

void OnModelDropdown(lv_event_t* event) {
    const int selected = s_ui.menu_state.selected_slot;
    const auto* target = codex_menu::SettingsTarget(s_ui.menu_state);
    if (!CanUseCodex() || !target || !target->synced || !target->model_ready ||
        s_ui.menu_state.draft_settings_loading || MenuVoiceBusy() || !s_ui.menu_state.can_set_model) return;
    const uint32_t choice = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (choice >= s_ui.menu_state.models.size()) return;
    if (!SendMenuAction("set_model", selected, s_ui.menu_state.models[choice].id)) RefreshCodexMenu(true);
}

void OnEffortReleased(lv_event_t* event) {
    const int selected = s_ui.menu_state.selected_slot;
    const auto* target = codex_menu::SettingsTarget(s_ui.menu_state);
    const auto* model = target ? codex_menu::FindModel(s_ui.menu_state, target->model) : nullptr;
    if (!CanUseCodex() || !model || model->efforts.empty() || !s_ui.menu_state.can_set_effort || MenuVoiceBusy() ||
        !s_ui.pending_menu_request.empty() || !s_ui.menu_state.connected || !target->synced || !target->effort_ready ||
        s_ui.menu_state.draft_settings_loading) { RefreshCodexMenu(true); return; }
    const uint32_t index = lv_slider_get_value(lv_event_get_target_obj(event));
    if (index >= model->efforts.size() || model->efforts[index].id == target->effort) return;
    if (!SendMenuAction("set_effort", selected, {}, model->efforts[index].id)) RefreshCodexMenu(true);
}

void OnFastChanged(lv_event_t* event) {
    const int selected = s_ui.menu_state.selected_slot;
    const auto* target = codex_menu::SettingsTarget(s_ui.menu_state);
    if (!CanUseCodex() || !target || !target->synced || !target->fast_ready ||
        s_ui.menu_state.draft_settings_loading || MenuVoiceBusy() || !s_ui.menu_state.can_set_fast) return;
    const bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    if (!SendMenuAction("set_fast", selected, {}, {}, enabled ? 1 : 0)) RefreshCodexMenu(true);
}

void OnRingChanged(lv_event_t* event) {
    s_ui.ring_enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    Settings settings("codex", true);
    settings.SetBool("status_ring", s_ui.ring_enabled);
    UpdateStatusRing();
}

void UpdateStatusRing() {
    if (s_ui.ring_frame == nullptr) return;
    const auto* target = codex_menu::SelectedTask(s_ui.menu_state);
    const auto state = target != nullptr ? target->state : codex_menu::SlotState::Unknown;
    const bool recording = s_ui.voice_stage == VoiceStage::Recording;
    const bool active = s_ui.ring_enabled && CanUseCodex() &&
        (recording || (s_ui.menu_state.connected &&
         (s_ui.task_active || state == codex_menu::SlotState::Working || state == codex_menu::SlotState::Waiting)));
    const bool terminal = s_ui.ring_enabled && CanUseCodex() &&
        s_ui.menu_state.connected && state == codex_menu::SlotState::Error;
    const auto& colors = Theme::Get().colors();
    const uint32_t color = terminal ? colors.danger : recording ? 0x2FAE7A : state == codex_menu::SlotState::Waiting ? colors.warning : colors.accent;
    codex_status_ring::Update(s_ui.ring_frame, active || terminal, active, color);
}

void RefreshCodexMenu(bool force) {
    if (!s_ui.menu_ui.overlay) return;
    codex_menu_ui::Refresh(s_ui.menu_ui, s_ui.menu_state, !s_ui.pending_menu_request.empty(), MenuVoiceBusy(), force);
}

void OnDeleted(lv_event_t*) {
    IdlePower::Get().SetMicroDisplay({});
    if (s_ui.pending_menu_request.empty()) s_task_entry.Remember(s_ui.menu_state);
    EndRealtimeSession(true);
    auto& client = CodexWsClient::GetInstance();
    client.SetOnMessageCallback({});
    client.SetOnStatusCallback({});
    client.SetOnDiscoveryCallback({});
    FinishVoiceCapture(true);
    client.SetAppActive(false);
    ResetStopHoldProgress();
    if (s_ui.menu_pending_timer) { lv_timer_delete(s_ui.menu_pending_timer); s_ui.menu_pending_timer = nullptr; }
    if (s_ui.bridge_status_timer) { lv_timer_delete(s_ui.bridge_status_timer); s_ui.bridge_status_timer = nullptr; }
    if (s_ui.realtime_timer) { lv_timer_delete(s_ui.realtime_timer); s_ui.realtime_timer = nullptr; }
    s_ui.stop_hold_progress = nullptr;
    CancelVoiceLabelTimer();
    controls::SetVoiceButtonAnimating(s_ui.voice_button, false);
    Keyboard::Get().Hide();
    s_ui.conversation_ui.reset();
    codex_notification::Get().SetRecording(false);
    s_ui.root.store(nullptr);
    s_ui.chat = nullptr;
    s_ui.action_button = nullptr;
    s_ui.task_actions = nullptr;
    s_ui.action_icon = nullptr;
    s_ui.action_label = nullptr;
    s_ui.stop_button = nullptr;
    s_ui.stop_label = nullptr;
    s_ui.voice_button = {};
    s_ui.config_overlay = nullptr;
    s_ui.discovery_name = nullptr;
    s_ui.connection_modes[0] = nullptr;
    s_ui.connection_modes[1] = nullptr;
    s_ui.connection_panels[0] = nullptr;
    s_ui.connection_panels[1] = nullptr;
    s_ui.connection_action = {};
    s_ui.remote_ip = nullptr;
    s_ui.token = nullptr;
    s_ui.voice_label_timer = nullptr;
    s_ui.model_summary = nullptr;
    s_ui.model_dropdown = nullptr;
    s_ui.fast_switch = nullptr;
    s_ui.menu_ui = {};
    s_ui.ring_frame = nullptr;
    s_ui.displayed_task.clear();
    s_ui.displayed_messages.clear();
    s_ui.displayed_ready = false;
    s_ui.conversation_initialized = false;
    for (size_t i = 0; i < 3; ++i) { s_ui.menu_tabs[i] = nullptr; s_ui.menu_panels[i] = nullptr; }
    for (size_t i = 0; i < 6; ++i) s_ui.task_cards[i] = nullptr;
    s_ui.new_task_button = nullptr;
    s_ui.realtime_button = nullptr;
    s_ui.realtime_root = nullptr;
    s_ui.realtime_canvas = nullptr;
    s_ui.realtime_status = nullptr;
    s_ui.realtime_mute = nullptr;
    s_ui.realtime_volume = nullptr;
    s_ui.realtime_end = nullptr;
    s_ui.voice_stage = VoiceStage::Idle;
    s_ui.voice_mode = VoiceMode::Micro;
    s_ui.voice_pressed = false;
    s_ui.voice_cancel_armed = false;
    s_ui.voice_cancel_pending = false;
    s_ui.voice_capture_started = false;
    s_ui.voice_request_id.clear();
    s_ui.connected_token.clear();
    s_ui.voice_target.clear();
    s_ui.stop_target.clear();
    s_ui.stop_request_id.clear();
    s_ui.stop_triggered = false;
    s_ui.connected_remote_ip.clear();
    s_ui.task_active = false;
    s_ui.stop_pending = false;
    s_ui.remote_mode = false;
    s_ui.connected_remote_mode = false;
    s_ui.voice_animation_active = false;
    s_ui.pending_menu_request.clear();
    s_ui.pending_new_task = false;
    s_ui.menu_state = {};
    s_ui.bridge_connection = {};
    s_ui.bridge_was_usable = false;
}

}  // namespace

lv_obj_t* CodexView::Create() {
    s_task_entry.Begin();
    auto shell = CreateAppShell("Codex", "自动发现 PC 服务");
    s_ui.task_actions = shell.actions;
    s_ui.root.store(shell.root);
    lv_obj_add_event_cb(shell.root, OnDeleted, LV_EVENT_DELETE, nullptr);

    s_ui.chat = lv_obj_create(shell.content);
    lv_obj_remove_style_all(s_ui.chat);
    lv_obj_set_size(s_ui.chat, 720, metrics::kBottomActionContentHeight);
    lv_obj_set_pos(s_ui.chat, 0, 0);
    lv_obj_set_style_pad_hor(s_ui.chat, metrics::kPagePadding, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_ui.chat, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_ui.chat, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.chat, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_ui.chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ui.chat, LV_SCROLLBAR_MODE_OFF);

    // Realtime is a dedicated, no-transcript surface. Its 600 px reference
    // field is drawn procedurally at 30 FPS from sparse 56x56 logical cells.
    s_ui.realtime_root = lv_obj_create(shell.content);
    lv_obj_remove_style_all(s_ui.realtime_root);
    lv_obj_set_size(s_ui.realtime_root, 720, metrics::kBottomActionContentHeight);
    lv_obj_set_pos(s_ui.realtime_root, 0, 0);
    lv_obj_set_style_bg_color(s_ui.realtime_root, lv_color_hex(Theme::Get().colors().background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.realtime_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.realtime_root, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.realtime_canvas = lv_obj_create(s_ui.realtime_root);
    lv_obj_remove_style_all(s_ui.realtime_canvas);
    lv_obj_set_size(s_ui.realtime_canvas, 600, 430);
    lv_obj_align(s_ui.realtime_canvas, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_add_event_cb(s_ui.realtime_canvas, RenderRealtimePixels, LV_EVENT_DRAW_MAIN, nullptr);
    s_ui.realtime_status = lv_label_create(s_ui.realtime_root);
    lv_label_set_text(s_ui.realtime_status, "连接中");
    lv_obj_set_style_text_font(s_ui.realtime_status, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.realtime_status, lv_color_hex(Theme::Get().colors().text), LV_PART_MAIN);
    lv_obj_align(s_ui.realtime_status, LV_ALIGN_TOP_MID, 0, 450);
    auto make_realtime_button = [&](const char* label, int x, lv_event_cb_t cb) {
        lv_obj_t* button = lv_btn_create(s_ui.realtime_root);
        lv_obj_set_size(button, 170, 58);
        lv_obj_align(button, LV_ALIGN_TOP_LEFT, x, 500);
        lv_obj_set_style_bg_color(button, lv_color_hex(Theme::Get().colors().surface), LV_PART_MAIN);
        lv_obj_t* text = lv_label_create(button);
        lv_label_set_text(text, label);
        lv_obj_center(text);
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);
        return button;
    };
    s_ui.realtime_mute = make_realtime_button("静音", 82, OnRealtimeMute);
    s_ui.realtime_volume = make_realtime_button("音量 70%", 275, OnRealtimeVolume);
    s_ui.realtime_end = make_realtime_button("结束", 468, OnRealtimeEnd);
    lv_obj_set_style_bg_color(s_ui.realtime_end, lv_color_hex(Theme::Get().colors().danger), LV_PART_MAIN);
    lv_obj_add_flag(s_ui.realtime_root, LV_OBJ_FLAG_HIDDEN);
    s_ui.realtime_timer = lv_timer_create([](lv_timer_t*) {
        if (s_ui.realtime_root != nullptr && !lv_obj_has_flag(s_ui.realtime_root, LV_OBJ_FLAG_HIDDEN) &&
            s_ui.realtime_canvas != nullptr) lv_obj_invalidate(s_ui.realtime_canvas);
    }, 33, nullptr);

    auto voice = controls::AddBottomVoiceButton(
        shell.actions, "按住说话", nullptr);
    s_ui.action_button = voice.root;
    s_ui.action_icon = voice.icon;
    s_ui.action_label = voice.label;
    s_ui.voice_button = voice;
    auto stop = controls::AddBottomPrimaryButton(
        shell.actions, FONT_AWESOME_STOP, "停止", nullptr, nullptr);
    lv_obj_set_width(voice.root, 0);
    lv_obj_set_width(stop.root, 0);
    lv_obj_set_flex_grow(voice.root, 2);
    lv_obj_set_flex_grow(stop.root, 1);
    lv_obj_set_style_bg_color(stop.root, lv_color_hex(Theme::Get().colors().danger), LV_PART_MAIN);
    lv_obj_set_style_bg_color(stop.root, lv_color_darken(lv_color_hex(Theme::Get().colors().danger), LV_OPA_20), LV_STATE_PRESSED);
    lv_obj_set_style_opa(stop.root, LV_OPA_50, LV_STATE_DISABLED);
    s_ui.stop_button = stop.root;
    s_ui.stop_label = stop.label;
    lv_obj_add_flag(stop.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(stop.root, OnStopPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(stop.root, OnStopReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(stop.root, OnStopReleased, LV_EVENT_PRESS_LOST, nullptr);
    s_ui.stop_hold_progress = lv_obj_create(stop.root);
    lv_obj_remove_style_all(s_ui.stop_hold_progress);
    lv_obj_set_size(s_ui.stop_hold_progress, 0, 7);
    lv_obj_align(s_ui.stop_hold_progress, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(
        s_ui.stop_hold_progress,
        lv_color_hex(Theme::Get().colors().accent_ink), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.stop_hold_progress, LV_OPA_COVER,
                            LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.stop_hold_progress, 4, LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.stop_hold_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(voice.root, OnVoicePressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_flag(voice.root, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(voice.root, OnVoicePressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(voice.root, OnVoiceReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(voice.root, OnVoiceReleased, LV_EVENT_PRESS_LOST, nullptr);
    auto menu = controls::AddBottomActionButton(
        shell.actions, nullptr, "菜单", OnOpenConfig);
    AddMenuGlyph(menu.root);

    auto& client = CodexWsClient::GetInstance();
    client.Init();
    BuildConfigDialog(shell.root);
    s_ui.conversation_ui = std::make_unique<CodexConversationUi>(s_ui.chat, shell.root,
        [](const std::string& json) {
            return CanUseCodex() && CodexWsClient::GetInstance().SendTextMessage(json, pdMS_TO_TICKS(200));
        },
        [](lv_obj_t* bubble, const char* text, bool user) { RenderMarkdown(bubble, text, user, Theme::Get().colors()); });
    s_ui.ring_frame = codex_status_ring::Create(shell.root);

    UiDispatcher::Init();
    client.SetOnMessageCallback(PostMessage);
    client.SetOnStatusCallback(PostStatus);
    client.SetOnDiscoveryCallback(PostDiscovery);
    client.SetAppActive(true);
    s_ui.bridge_connection.SetTransportConnected(client.IsConnected(), lv_tick_get());
    s_ui.bridge_status_timer = lv_timer_create([](lv_timer_t*) {
        RefreshBridgeUi(lv_tick_get());
    }, 1000, nullptr);
    RefreshBridgeUi(lv_tick_get());
    client.SetDiscoveryEnabled(!s_ui.remote_mode);
    if (!client.IsConnected()) client.StartDiscovery(8000);
    if (!client.HasToken()) ShowConfig();
    SyncConversation();
    UpdateStatusRing();
    return shell.root;
}

}  // namespace agent_ui
