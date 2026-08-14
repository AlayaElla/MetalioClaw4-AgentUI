#include "codex_view.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>

#include <esp_log.h>
#include <font_awesome.h>

#include "cJSON.h"
#include "codex_ws_client.h"
#include "application.h"
#include "core/app_shell.h"
#include "core/fonts.h"
#include "core/theme.h"
#include "ui_dispatcher.h"
#include "components/system_keyboard.h"
#include "components/ui_components.h"

extern "C" void codex_remote_play_stop_sound();

namespace agent_ui {
namespace {

namespace controls = ui_components;

constexpr char kTag[] = "AgentCodex";
constexpr int kMaxMessages = 20;
constexpr int kUserBubbleWidth = 590;
constexpr int kAssistantBubbleWidth = 620;
// Voice capture starts as soon as the press is received.  Only the destructive
// stop action is protected by a deliberately slower hold gesture.
constexpr uint32_t kStopHoldDurationMs = 1200;
constexpr uint32_t kStopHoldUpdateMs = 20;

enum class VoiceStage {
    Idle,
    Preparing,
    Recording,
    AwaitingRecognition,
    Recognizing,
    Submitting,
    Error,
};

struct UiState {
    std::atomic<lv_obj_t*> root{nullptr};
    lv_obj_t* chat = nullptr;
    lv_obj_t* status_dot = nullptr;
    lv_obj_t* status_text = nullptr;
    lv_obj_t* action_button = nullptr;
    lv_obj_t* action_icon = nullptr;
    lv_obj_t* action_label = nullptr;
    controls::VoiceButtonParts voice_button{};
    lv_obj_t* stop_hold_progress = nullptr;
    lv_timer_t* stop_hold_timer = nullptr;
    lv_obj_t* config_overlay = nullptr;
    lv_obj_t* discovery_name = nullptr;
    lv_obj_t* connection_modes[2]{};
    lv_obj_t* connection_panels[2]{};
    controls::ActionButtonParts connection_action{};
    lv_obj_t* remote_ip = nullptr;
    lv_obj_t* token = nullptr;
    lv_obj_t* approval = nullptr;
    lv_obj_t* approval_question = nullptr;
    lv_timer_t* voice_label_timer = nullptr;
    std::string discovered_name;
    std::string discovered_ip;
    std::string approval_id;
    std::string voice_request_id;
    std::string connected_token;
    std::string connected_remote_ip;
    int discovered_port = 8765;
    uint32_t request_counter = 0;
    uint32_t stop_hold_started_at = 0;
    VoiceStage voice_stage = VoiceStage::Idle;
    bool voice_pressed = false;
    bool task_active = false;
    bool stop_pending = false;
    bool remote_mode = false;
    bool connected_remote_mode = false;
    bool voice_animation_active = false;
};

UiState s_ui;

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

void SetStatus(bool connected, const char* text = nullptr) {
    const auto& colors = Theme::Get().colors();
    if (s_ui.status_dot != nullptr) {
        lv_obj_set_style_bg_color(
            s_ui.status_dot,
            lv_color_hex(connected ? colors.accent : colors.danger), LV_PART_MAIN);
    }
    if (s_ui.status_text != nullptr) {
        lv_label_set_text(s_ui.status_text,
                          text != nullptr ? text : (connected ? "已连接" : "未连接"));
    }
}

void UpdateActionButton() {
    if (s_ui.action_button == nullptr || s_ui.action_icon == nullptr) return;
    const auto& colors = Theme::Get().colors();
    uint32_t color = colors.accent;
    const char* icon = FONT_AWESOME_MICROPHONE;
    if (s_ui.task_active) {
        color = colors.danger;
        icon = FONT_AWESOME_STOP;
    } else if (s_ui.voice_stage != VoiceStage::Idle) {
        color = colors.warning;
        icon = s_ui.voice_stage == VoiceStage::Recording
                   ? FONT_AWESOME_STOP
                   : FONT_AWESOME_MICROPHONE;
    }
    lv_obj_set_style_bg_color(s_ui.action_button, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(s_ui.action_icon, icon);
    if (s_ui.action_label != nullptr) {
        const char* label = "按住说话";
        if (s_ui.task_active) {
            label = "停止任务";
        } else if (s_ui.voice_stage == VoiceStage::Preparing ||
                   s_ui.voice_stage == VoiceStage::AwaitingRecognition) {
            label = "请稍后...";
        } else if (s_ui.voice_stage == VoiceStage::Recording) {
            label = "正在录音…松开发送";
        } else if (s_ui.voice_stage == VoiceStage::Recognizing) {
            label = "正在识别...";
        } else if (s_ui.voice_stage == VoiceStage::Submitting) {
            label = "正在发送...";
        } else if (s_ui.voice_stage == VoiceStage::Error) {
            label = "识别失败";
        }
        lv_label_set_text(s_ui.action_label, label);
    }
    const bool animate_voice =
        !s_ui.task_active && s_ui.voice_stage != VoiceStage::Idle &&
        s_ui.voice_stage != VoiceStage::Error;
    if (s_ui.voice_animation_active != animate_voice) {
        controls::SetVoiceButtonAnimating(s_ui.voice_button, animate_voice);
        s_ui.voice_animation_active = animate_voice;
    }
}

void SetVoiceStage(VoiceStage stage) {
    s_ui.voice_stage = stage;
    UpdateActionButton();
}

void CancelVoiceLabelTimer();
void ResetStopHoldProgress();

void SetTaskActive(bool active) {
    s_ui.task_active = active;
    if (!active) {
        s_ui.stop_pending = false;
        ResetStopHoldProgress();
    }
    UpdateActionButton();
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

void AddMessage(const char* role, const char* text) {
    if (s_ui.chat == nullptr || text == nullptr || text[0] == '\0') return;
    const bool user = role != nullptr && std::strcmp(role, "user") == 0;
    const auto& colors = Theme::Get().colors();

    lv_obj_t* row = lv_obj_create(s_ui.chat);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_bottom(row, 10, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, user ? kUserBubbleWidth : kAssistantBubbleWidth);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(bubble, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, 22, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, metrics::kRadiusPanel, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        bubble, lv_color_hex(user ? colors.accent : colors.raised), LV_PART_MAIN);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bubble, 10, LV_PART_MAIN);
    lv_obj_align(bubble, user ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT,
                 user ? -4 : 4, 0);
    RenderMarkdown(bubble, text, user, colors);

    while (lv_obj_get_child_count(s_ui.chat) > kMaxMessages) {
        lv_obj_delete(lv_obj_get_child(s_ui.chat, 0));
    }
    lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

void ClearMessages() {
    if (s_ui.chat != nullptr) lv_obj_clean(s_ui.chat);
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
    }
}

void ShowConfig() {
    if (s_ui.config_overlay == nullptr) return;
    lv_obj_remove_flag(s_ui.config_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.config_overlay);
    if (s_ui.discovery_name != nullptr) {
        lv_label_set_text(
            s_ui.discovery_name,
            s_ui.discovered_name.empty() ? "正在自动发现 PC 服务..."
                                         : s_ui.discovered_name.c_str());
    }
    if (!s_ui.remote_mode) CodexWsClient::GetInstance().StartDiscovery(8000);
}

void ShowApproval(const char* question, const char* id) {
    if (s_ui.approval == nullptr || s_ui.approval_question == nullptr) return;
    s_ui.approval_id = id != nullptr ? id : "";
    lv_label_set_text(s_ui.approval_question, question != nullptr ? question : "");
    lv_obj_remove_flag(s_ui.approval, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.approval);
}

void HandleMessage(const std::string& message) {
    cJSON* root = cJSON_Parse(message.c_str());
    if (root == nullptr) return;
    const cJSON* type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "";

    if (std::strcmp(type, "chat") == 0) {
        const cJSON* role = cJSON_GetObjectItemCaseSensitive(root, "role");
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text)) {
            AddMessage(cJSON_IsString(role) ? role->valuestring : "codex",
                       text->valuestring);
        }
    } else if (std::strcmp(type, "status") == 0) {
        const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(state)) {
            if (IsWorkingState(state->valuestring)) SetTaskActive(true);
            if (IsIdleState(state->valuestring)) SetTaskActive(false);
        }
    } else if (std::strcmp(type, "voice_status") == 0) {
        const cJSON* request_id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
        if (cJSON_IsString(request_id) && !s_ui.voice_request_id.empty() &&
            s_ui.voice_request_id != request_id->valuestring) {
            cJSON_Delete(root);
            return;
        }
        const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(state)) {
            if (std::strcmp(state->valuestring, "preparing") == 0) {
                SetVoiceStage(VoiceStage::Preparing);
            } else if (std::strcmp(state->valuestring, "recording") == 0) {
                if (s_ui.voice_pressed) {
                    Application::GetInstance().StartCodexVoiceCapture();
                    SetVoiceStage(VoiceStage::Recording);
                }
            } else if (std::strcmp(state->valuestring, "recognizing") == 0) {
                SetVoiceStage(VoiceStage::Recognizing);
            } else if (std::strcmp(state->valuestring, "submitting") == 0) {
                SetVoiceStage(VoiceStage::Submitting);
            } else if (std::strcmp(state->valuestring, "submitted") == 0) {
                s_ui.voice_request_id.clear();
                SetVoiceStage(VoiceStage::Idle);
                SetTaskActive(true);
            } else if (std::strcmp(state->valuestring, "error") == 0) {
                Application::GetInstance().StopCodexVoiceCapture();
                s_ui.voice_pressed = false;
                s_ui.voice_request_id.clear();
                SetVoiceStage(VoiceStage::Error);
                CancelVoiceLabelTimer();
                s_ui.voice_label_timer = lv_timer_create(
                    [](lv_timer_t* timer) {
                        s_ui.voice_label_timer = nullptr;
                        SetVoiceStage(VoiceStage::Idle);
                        lv_timer_delete(timer);
                    },
                    1600, nullptr);
            }
        }
    } else if (std::strcmp(type, "stop") == 0) {
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text)) AddMessage("codex", text->valuestring);
        SetTaskActive(false);
        codex_remote_play_stop_sound();
    } else if (std::strcmp(type, "task_new_result") == 0) {
        const cJSON* success = cJSON_GetObjectItemCaseSensitive(root, "success");
        if (cJSON_IsTrue(success)) {
            ClearMessages();
            SetTaskActive(false);
        }
    } else if (std::strcmp(type, "approval_request") == 0) {
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
        const cJSON* question = cJSON_GetObjectItemCaseSensitive(root, "question");
        if (cJSON_IsString(id) && cJSON_IsString(question)) {
            ShowApproval(question->valuestring, id->valuestring);
        }
    }
    cJSON_Delete(root);
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
        SetStatus(connected);
        if (connected) {
            CaptureConnectedConfig();
            HideConfig();
        }
        if (!connected) {
            if (s_ui.voice_label_timer != nullptr) {
                lv_timer_delete(s_ui.voice_label_timer);
                s_ui.voice_label_timer = nullptr;
            }
            Application::GetInstance().StopCodexVoiceCapture();
            s_ui.voice_stage = VoiceStage::Idle;
            s_ui.voice_pressed = false;
            s_ui.voice_request_id.clear();
            SetTaskActive(false);
            UpdateConnectionAction();
        }
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

void FinishVoiceCapture() {
    if (s_ui.voice_stage != VoiceStage::Preparing &&
        s_ui.voice_stage != VoiceStage::Recording) return;
    const bool was_recording = s_ui.voice_stage == VoiceStage::Recording;
    s_ui.voice_pressed = false;
    SetVoiceStage(VoiceStage::AwaitingRecognition);
    const std::string request_id = s_ui.voice_request_id;
    auto send_voice_end = [request_id]() {
        auto& ws = CodexWsClient::GetInstance();
        ws.SendTextMessage("{\"type\":\"voice_end\",\"requestId\":\"" +
                           request_id + "\"}");
    };
    if (was_recording) {
        Application::GetInstance().StopCodexVoiceCapture(std::move(send_voice_end));
    } else {
        send_voice_end();
    }
}

void StartVoiceCapture() {
    auto& client = CodexWsClient::GetInstance();
    if (!client.IsConnected()) {
        ShowConfig();
        return;
    }
    if (s_ui.task_active || s_ui.voice_stage != VoiceStage::Idle) return;
    CancelVoiceLabelTimer();
    s_ui.voice_request_id = "esp32-voice-" +
                            std::to_string(++s_ui.request_counter);
    if (!client.SendTextMessage(
            "{\"type\":\"voice_start\",\"requestId\":\"" +
            s_ui.voice_request_id + "\"}")) {
        s_ui.voice_request_id.clear();
        return;
    }
    s_ui.voice_pressed = true;
    SetVoiceStage(VoiceStage::Preparing);
}

void ResetStopHoldProgress() {
    if (s_ui.stop_hold_timer != nullptr) {
        lv_timer_delete(s_ui.stop_hold_timer);
        s_ui.stop_hold_timer = nullptr;
    }
    if (s_ui.stop_hold_progress != nullptr) {
        lv_obj_set_width(s_ui.stop_hold_progress, 0);
    }
}

void SendStopRequest() {
    if (!s_ui.task_active || s_ui.stop_pending) return;
    const std::string id = "esp32-stop-" +
                           std::to_string(++s_ui.request_counter);
    if (CodexWsClient::GetInstance().SendTextMessage(
            "{\"type\":\"turn_stop\",\"requestId\":\"" + id + "\"}")) {
        s_ui.stop_pending = true;
    }
}

void OnStopHoldTimer(lv_timer_t*) {
    if (!s_ui.task_active || s_ui.stop_pending) {
        ResetStopHoldProgress();
        return;
    }
    const uint32_t elapsed = lv_tick_elaps(s_ui.stop_hold_started_at);
    const uint32_t progress = std::min<uint32_t>(
        100, elapsed * 100 / kStopHoldDurationMs);
    if (s_ui.stop_hold_progress != nullptr) {
        lv_obj_set_width(s_ui.stop_hold_progress, LV_PCT(progress));
    }
    if (elapsed < kStopHoldDurationMs) return;

    if (s_ui.stop_hold_timer != nullptr) {
        lv_timer_delete(s_ui.stop_hold_timer);
        s_ui.stop_hold_timer = nullptr;
    }
    SendStopRequest();
}

void StartStopHold() {
    if (s_ui.stop_hold_timer != nullptr || s_ui.stop_pending) return;
    s_ui.stop_hold_started_at = lv_tick_get();
    if (s_ui.stop_hold_progress != nullptr) {
        // Make the progress affordance visible on the initial press; the timer
        // then advances it linearly until the stop request is sent.
        lv_obj_set_width(s_ui.stop_hold_progress, 1);
    }
    s_ui.stop_hold_timer = lv_timer_create(
        OnStopHoldTimer, kStopHoldUpdateMs, nullptr);
}

void OnVoicePressed(lv_event_t*) {
    auto& client = CodexWsClient::GetInstance();
    if (!client.IsConnected()) {
        ShowConfig();
        return;
    }
    if (s_ui.task_active) {
        StartStopHold();
        return;
    }
    if (s_ui.voice_stage != VoiceStage::Idle) {
        return;
    }
    // Starting capture is intentionally immediate.  The websocket's
    // preparing/recording states drive the existing voice UI while the
    // pointer remains pressed; there is no pre-capture hold progress.
    StartVoiceCapture();
}

void OnVoiceReleased(lv_event_t*) {
    if (s_ui.task_active) {
        ResetStopHoldProgress();
        return;
    }
    FinishVoiceCapture();
}

void OnNewTask(lv_event_t*) {
    auto& client = CodexWsClient::GetInstance();
    ClearMessages();
    SetTaskActive(false);
    HideConfig();
    if (client.IsConnected()) {
        const std::string id =
            "esp32-task-" + std::to_string(++s_ui.request_counter);
        client.SendTextMessage("{\"type\":\"task_new\",\"requestId\":\"" +
                               id + "\"}");
    }
}

void OnSaveToken(lv_event_t*) {
    if (s_ui.token == nullptr) return;
    auto& client = CodexWsClient::GetInstance();
    if (client.IsConnected() && !HasNewConnectionInfo()) {
        HideConfig();
        return;
    }
    const char* token = lv_textarea_get_text(s_ui.token);
    if (token == nullptr || !client.SaveToken(token)) return;
    Keyboard::Get().Hide();
    if (s_ui.remote_mode) {
        const char* remote_ip = s_ui.remote_ip != nullptr
                                    ? lv_textarea_get_text(s_ui.remote_ip)
                                    : nullptr;
        if (remote_ip == nullptr || remote_ip[0] == '\0') return;
        client.Connect(remote_ip, 8765);
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

void SendApproval(const char* decision) {
    if (s_ui.approval_id.empty()) return;
    CodexWsClient::GetInstance().SendTextMessage(
        "{\"type\":\"approval\",\"id\":\"" + s_ui.approval_id +
        "\",\"decision\":\"" + decision + "\"}");
    if (s_ui.approval != nullptr) {
        lv_obj_add_flag(s_ui.approval, LV_OBJ_FLAG_HIDDEN);
    }
}

void OnApprove(lv_event_t*) { SendApproval("allow"); }
void OnReject(lv_event_t*) { SendApproval("deny"); }
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

lv_obj_t* CreateDialogButton(lv_obj_t* parent, const char* text, bool accent,
                             lv_event_cb_t callback) {
    lv_obj_t* button = controls::CreateButton(parent);
    StyleButton(button, accent);
    lv_obj_set_size(button, 210, 58);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    StyleLabel(label);
    if (accent) {
        lv_obj_set_style_text_color(label,
                                    lv_color_hex(Theme::Get().colors().accent_ink),
                                    LV_PART_MAIN);
    }
    lv_obj_center(label);
    return button;
}

void BuildConfigDialog(lv_obj_t* root) {
    const auto& colors = Theme::Get().colors();
    s_ui.config_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(s_ui.config_overlay);
    lv_obj_set_size(s_ui.config_overlay, 720, 658);
    lv_obj_set_pos(s_ui.config_overlay, 0, metrics::kStatusBarHeight);
    lv_obj_set_style_bg_color(s_ui.config_overlay, lv_color_black(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.config_overlay, LV_OPA_30, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.config_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        s_ui.config_overlay,
        [](lv_event_t* event) {
            if (lv_event_get_target_obj(event) ==
                lv_event_get_current_target_obj(event)) {
                HideConfig();
            }
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_ui.config_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* drawer = lv_obj_create(s_ui.config_overlay);
    lv_obj_remove_style_all(drawer);
    lv_obj_set_size(drawer, 540, 658);
    lv_obj_align(drawer, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(drawer, lv_color_hex(colors.background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(drawer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(drawer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = controls::CreateContentPanel(
        drawer, metrics::kBottomActionContentHeight, 12);
    lv_obj_set_size(panel, 480, metrics::kBottomActionContentHeight);
    lv_obj_set_pos(panel, 30, 0);
    lv_obj_set_style_pad_top(panel, 18, LV_PART_MAIN);

    auto task = controls::CreateCompactRow(
        panel, FONT_AWESOME_PEN_TO_SQUARE, "新建任务",
        nullptr, nullptr, 78, false, false,
        OnNewTask);
    lv_obj_set_style_bg_color(task.root, lv_color_hex(colors.raised), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(task.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(task.root, metrics::kRadiusControl, LV_PART_MAIN);

    controls::CreateSectionHeading(panel, "连接设置");
    lv_obj_t* modes = controls::CreateSegment(panel, 68);
    s_ui.connection_modes[0] = controls::AddSegmentButton(
        modes, FONT_AWESOME_WIFI, "局域网", true, OnConnectionMode,
        reinterpret_cast<void*>(0));
    s_ui.connection_modes[1] = controls::AddSegmentButton(
        modes, FONT_AWESOME_SIGNAL, "公网", false, OnConnectionMode,
        reinterpret_cast<void*>(1));

    s_ui.connection_panels[0] = controls::CreateContentPanel(panel, 86);
    auto device = controls::CreateCompactRow(
        s_ui.connection_panels[0], FONT_AWESOME_LINK,
        "正在自动发现 PC 服务...", nullptr, nullptr, 86,
        false, false);
    lv_obj_set_style_border_color(device.root, lv_color_hex(colors.border),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(device.root, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(device.root, metrics::kRadiusControl, LV_PART_MAIN);
    s_ui.discovery_name = device.title;

    s_ui.connection_panels[1] = controls::CreateContentPanel(panel, 86);
    lv_obj_add_flag(s_ui.connection_panels[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* remote_label = lv_label_create(s_ui.connection_panels[1]);
    lv_label_set_text(remote_label, "公网 IP 地址");
    lv_obj_set_style_text_font(remote_label, fonts::Small(), LV_PART_MAIN);
    lv_obj_set_style_text_color(remote_label, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_align(remote_label, LV_ALIGN_TOP_LEFT, 0, 0);
    s_ui.remote_ip = lv_textarea_create(s_ui.connection_panels[1]);
    lv_obj_set_size(s_ui.remote_ip, LV_PCT(100), 58);
    lv_obj_align(s_ui.remote_ip, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_textarea_set_one_line(s_ui.remote_ip, true);
    lv_textarea_set_placeholder_text(s_ui.remote_ip, "例如 203.0.113.10");
    lv_obj_set_style_text_font(s_ui.remote_ip, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.remote_ip, metrics::kRadiusControl, LV_PART_MAIN);
    lv_obj_add_event_cb(s_ui.remote_ip, OnConnectionInfoChanged,
                        LV_EVENT_VALUE_CHANGED, nullptr);
    Keyboard::Get().Bind(s_ui.remote_ip, "公网 IP", LV_KEYBOARD_MODE_TEXT_LOWER);

    lv_obj_t* token_field = controls::CreateContentPanel(panel, 98);
    lv_obj_t* token_label = lv_label_create(token_field);
    lv_label_set_text(token_label, "认证 Token");
    lv_obj_set_style_text_font(token_label, fonts::Small(), LV_PART_MAIN);
    lv_obj_set_style_text_color(token_label, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_align(token_label, LV_ALIGN_TOP_LEFT, 0, 0);
    s_ui.token = lv_textarea_create(token_field);
    lv_obj_set_size(s_ui.token, LV_PCT(100), 66);
    lv_obj_align(s_ui.token, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_textarea_set_one_line(s_ui.token, true);
    lv_textarea_set_password_mode(s_ui.token, true);
    lv_textarea_set_placeholder_text(s_ui.token, "输入认证 Token");
    lv_obj_set_style_text_font(s_ui.token, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.token, metrics::kRadiusControl, LV_PART_MAIN);
    std::string saved_token;
    if (CodexWsClient::GetInstance().LoadToken(saved_token)) {
        lv_textarea_set_text(s_ui.token, saved_token.c_str());
    }
    lv_obj_add_event_cb(s_ui.token, OnConnectionInfoChanged,
                        LV_EVENT_VALUE_CHANGED, nullptr);
    Keyboard::Get().Bind(s_ui.token, "Codex Token");

    lv_obj_t* actions = controls::CreateBottomActionBar(
        drawer, metrics::kBottomActionContentHeight);
    controls::AddBottomActionSpacer(actions);
    s_ui.connection_action = controls::AddBottomPrimaryButton(
        actions, FONT_AWESOME_LINK, "连接", OnSaveToken);
    CaptureConnectedConfig();
}

void BuildApprovalDialog(lv_obj_t* root) {
    s_ui.approval = lv_obj_create(root);
    StyleSurface(s_ui.approval, true);
    lv_obj_set_size(s_ui.approval, 600, 300);
    lv_obj_center(s_ui.approval);
    lv_obj_set_style_pad_all(s_ui.approval, 24, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.approval, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* title = lv_label_create(s_ui.approval);
    lv_label_set_text(title, "操作确认");
    lv_obj_set_style_text_font(title, fonts::Large(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(Theme::Get().colors().text),
                                LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_ui.approval_question = lv_label_create(s_ui.approval);
    StyleLabel(s_ui.approval_question);
    lv_obj_set_width(s_ui.approval_question, 552);
    lv_label_set_long_mode(s_ui.approval_question, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ui.approval_question, 0, 58);

    lv_obj_t* allow = CreateDialogButton(s_ui.approval, "允许", true, OnApprove);
    lv_obj_align(allow, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t* deny = CreateDialogButton(s_ui.approval, "拒绝", false, OnReject);
    lv_obj_align(deny, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

void OnDeleted(lv_event_t*) {
    auto& client = CodexWsClient::GetInstance();
    client.SetOnMessageCallback({});
    client.SetOnStatusCallback({});
    client.SetOnDiscoveryCallback({});
    if (s_ui.voice_stage != VoiceStage::Idle) {
        const std::string request_id = s_ui.voice_request_id;
        Application::GetInstance().StopCodexVoiceCapture([request_id]() {
            if (request_id.empty()) return;
            CodexWsClient::GetInstance().SendTextMessage(
                "{\"type\":\"voice_end\",\"requestId\":\"" +
                request_id + "\"}");
        });
    }
    ResetStopHoldProgress();
    s_ui.stop_hold_progress = nullptr;
    CancelVoiceLabelTimer();
    controls::SetVoiceButtonAnimating(s_ui.voice_button, false);
    Keyboard::Get().Hide();
    s_ui.root.store(nullptr);
    s_ui.chat = nullptr;
    s_ui.status_dot = nullptr;
    s_ui.status_text = nullptr;
    s_ui.action_button = nullptr;
    s_ui.action_icon = nullptr;
    s_ui.action_label = nullptr;
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
    s_ui.approval = nullptr;
    s_ui.approval_question = nullptr;
    s_ui.voice_label_timer = nullptr;
    s_ui.voice_stage = VoiceStage::Idle;
    s_ui.voice_pressed = false;
    s_ui.voice_request_id.clear();
    s_ui.connected_token.clear();
    s_ui.connected_remote_ip.clear();
    s_ui.task_active = false;
    s_ui.stop_pending = false;
    s_ui.remote_mode = false;
    s_ui.connected_remote_mode = false;
    s_ui.voice_animation_active = false;
}

}  // namespace

lv_obj_t* CodexView::Create() {
    auto shell = CreateAppShell("Codex", "自动发现 PC 服务");
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

    auto voice = controls::AddBottomVoiceButton(
        shell.actions, "按住说话", nullptr);
    s_ui.action_button = voice.root;
    s_ui.action_icon = voice.icon;
    s_ui.action_label = voice.label;
    s_ui.voice_button = voice;
    s_ui.stop_hold_progress = lv_obj_create(voice.root);
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
    lv_obj_add_event_cb(voice.root, OnVoiceReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(voice.root, OnVoiceReleased, LV_EVENT_PRESS_LOST, nullptr);
    auto menu = controls::AddBottomActionButton(
        shell.actions, nullptr, "菜单", OnOpenConfig);
    AddMenuGlyph(menu.root);

    auto& client = CodexWsClient::GetInstance();
    client.Init();
    BuildConfigDialog(shell.root);
    BuildApprovalDialog(shell.root);

    UiDispatcher::Init();
    client.SetOnMessageCallback(PostMessage);
    client.SetOnStatusCallback(PostStatus);
    client.SetOnDiscoveryCallback(PostDiscovery);
    SetStatus(client.IsConnected());
    if (!client.IsConnected()) client.StartDiscovery(8000);
    if (!client.HasToken()) ShowConfig();
    return shell.root;
}

}  // namespace agent_ui
