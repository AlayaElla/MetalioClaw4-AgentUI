#include "phone_view.h"
#include "i18n.h"

#include "core/app_shell.h"
#include "components/ui_components.h"
#include "core/fonts.h"
#include "core/navigation.h"
#include "core/theme.h"
#include "core/ui_utils.h"
#include <font_awesome.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "lvgl.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "IOExpander.hpp"
#include "board.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// 720x720 layout
//
//  +-----------------------------------------------+ y=0
//  |  [◀]  电话                                    |  header (h=88)
//  +-----------------------------------------------+ y=88
//  |              138 1234 5678          ⌫         |  number display (h=100)
//  |              (status line)                     |
//  +-----------------------------------------------+ y=188
//  |   [1]   [2 ABC]   [3 DEF]                      |
//  |   [4 GHI] [5 JKL] [6 MNO]                      |  digit grid 3x3
//  |   [7 PQRS][8 TUV] [9 WXYZ]                     |  + 0 居中
//  |            [0 +]              [ call / hangup ]|  拨打：右下角放大
//  +-----------------------------------------------+ y=720
// ---------------------------------------------------------------------------

namespace agent_ui {

namespace {

constexpr const char* TAG = "PhoneView";

constexpr int kPanelWidth   = agent_ui::metrics::kDisplaySize;
constexpr int kPanelHeight  = agent_ui::metrics::kBottomActionContentHeight;
constexpr int kPad          = agent_ui::metrics::kPagePadding;
constexpr int kHeaderH      = 0;
constexpr int kNumberAreaH  = 100;  // 容纳 50px 号码字 + 状态行
constexpr int kKeypadY      = kHeaderH + kNumberAreaH;
constexpr int kKeypadH      = kPanelHeight - kKeypadY;

// ----- number area sub-layout ----------------------------------------------
// We hand-place the number label, the status line, and the backspace button
// so the backspace's vertical center sits exactly on the number's vertical
// center -- making the trio read as a single horizontal row.
constexpr int kNumberLblTop = 8;
constexpr int kStatusLblTop = 70;
constexpr int kStatusLblH   = 24;

// 数字区：3 列 × 4 行（末行仅 0 居中）；拨打键贴屏幕右下角放大。
constexpr int kDigitRows    = 4;
constexpr int kKeypadCols   = 3;
constexpr int kKeypadSidePad = 58;
constexpr int kKeypadVerticalPad = 8;

// ----- keypad table ---------------------------------------------------------
struct KeyDef {
    const char* digit;
    const char* sub;     // letters (e.g. "ABC"), may be empty
    int row, col;
};

// 末行仅保留 0（中间列）；拨打键固定在屏幕右下角（见 BuildKeypad）。
const KeyDef kKeys[] = {
    {"1", "",     0, 0}, {"2", "ABC",  0, 1}, {"3", "DEF",  0, 2},
    {"4", "GHI",  1, 0}, {"5", "JKL",  1, 1}, {"6", "MNO",  1, 2},
    {"7", "PQRS", 2, 0}, {"8", "TUV",  2, 1}, {"9", "WXYZ", 2, 2},
    {"*", "",     3, 0}, {"0", "+",    3, 1}, {"#", "",     3, 2},
};

// ----- dialer state ---------------------------------------------------------
enum class CallState { kIdle, kCalling };

constexpr int  kMaxDigits   = 24;
char           s_number[kMaxDigits + 1];
CallState      s_call_state;

lv_obj_t* s_number_lbl;
lv_obj_t* s_status_lbl;
lv_obj_t* s_backspace_btn;
lv_obj_t* s_action_btn;        // 右下角：拨打 / 挂断
lv_obj_t* s_action_icon;       // image inside action_btn (dial / hangup)
lv_obj_t* s_action_label;

// Tracks whether the call screen is currently mounted. lv_async_call() trampolines
// from the AT-task thread back to the LVGL thread; if the user already swiped back
// we MUST NOT touch the now-deleted UI objects.
bool s_screen_active = false;

// Bumped on every call-state transition. Carried by the AT task; on completion
// we drop the result if the epoch no longer matches (e.g. user swiped back, or
// hung up before ATD returned). This avoids a stale "dial OK" overwriting a
// freshly-idle UI.
uint32_t s_call_epoch = 0;

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

// Format a Chinese-style phone number: "13812345678" -> "138 1234 5678".
// For non-11-digit input we just show it as typed (still grouped lightly).
void FormatNumberForDisplay(const char* in, char* out, size_t out_sz) {
    size_t len = std::strlen(in);
    if (len == 0) {
        if (out_sz > 0) out[0] = '\0';
        return;
    }
    // Mobile-style: 3-4-4 grouping when exactly 11 digits.
    if (len == 11) {
        snprintf(out, out_sz, "%.3s %.4s %.4s", in, in + 3, in + 7);
        return;
    }
    // Otherwise insert a space every 4 chars from the left for readability.
    size_t out_len = 0;
    for (size_t i = 0; i < len && out_len + 1 < out_sz; ++i) {
        if (i > 0 && i % 4 == 0 && out_len + 1 < out_sz) {
            out[out_len++] = ' ';
        }
        out[out_len++] = in[i];
    }
    if (out_len < out_sz) out[out_len] = '\0';
}

void RefreshNumberDisplay() {
    char formatted[40];
    FormatNumberForDisplay(s_number, formatted, sizeof(formatted));
    if (formatted[0] == '\0') {
        // Placeholder hint when nothing has been typed.
        lv_label_set_text(s_number_lbl, "");
    } else {
        lv_label_set_text(s_number_lbl, formatted);
    }
}

void RefreshActionButton() {
    const auto& colors = Theme::Get().colors();
    if (s_call_state == CallState::kCalling) {
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(colors.danger),
                                  LV_PART_MAIN);
        if (s_action_icon != nullptr) lv_label_set_text(s_action_icon, FONT_AWESOME_PHONE);
        if (s_action_label != nullptr) lv_label_set_text(s_action_label, I18n::T("挂断"));
    } else {
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(colors.accent),
                                  LV_PART_MAIN);
        if (s_action_icon != nullptr) lv_label_set_text(s_action_icon, FONT_AWESOME_PHONE);
        if (s_action_label != nullptr) lv_label_set_text(s_action_label, I18n::T("呼叫"));
    }
    lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(colors.accent_pressed),
                              LV_PART_MAIN | LV_STATE_PRESSED);
}

void RefreshStatus() {
    if (s_call_state == CallState::kCalling) {
        if (s_number[0] != '\0') {
            lv_label_set_text(s_status_lbl, I18n::T("拨号中..."));
        } else {
            lv_label_set_text(s_status_lbl, "");
        }
    } else {
        lv_label_set_text(s_status_lbl, "");
    }
}

void SetStatusText(const char* txt) {
    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, txt);
    }
}

// ---------------------------------------------------------------------------
// AT command plumbing
//
// 拨号 / 挂断按钮按下后，要把 AT 命令丢到一个独立 FreeRTOS task 里跑，
// 因为 UartEthModem::SendAt() 是同步阻塞的（最多 timeout_ms），不能在
// LVGL 主线程里直接调用，否则界面会卡住。
//
// 拨号流程会先发一条 AT+CPIN? 体检：
//   - 模组返回 "+CPIN: READY" 且带 OK 才继续 ATD<num>
//   - 否则提示 "请检查移动网络"，回 idle
//
// 任务结果通过 lv_async_call() 切回 LVGL 线程刷新 UI，
// 同时用 s_screen_active + s_call_epoch 双重保护，避免在屏幕已经销毁
// 或者状态已经变化（用户中途挂断/退出）时还去操作野指针。
// ---------------------------------------------------------------------------

Nt26Board* GetNt26Board() {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    if (dual != nullptr) {
        return dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    }
    return dynamic_cast<Nt26Board*>(&board);
}

// 读取 network_screen 写入的 NVS 偏好。0 = 外置卡（默认）、1 = 内置卡。
// 该值在 Network App 加载时通过 AT+ECSIMCFG? 与模组真实状态同步，所以
// 这里直接读 NVS 即可，无需在拨号关键路径上再发一次 AT 查询。
int GetSavedSimSlot() {
    Settings settings("network", true);
    int v = settings.GetInt("sim_slot", 0);
    return (v == 1) ? 1 : 0;
}

// 内置卡（SimSlot=1）出厂被定义为「数据卡」，没开 CS 语音业务，
// 直接 ATD 会被模组拒（NO CARRIER）。拨号前在 UI 层就拦下来，给用户
// 一条明确指引。
bool IsInternalSimActive() {
    return GetSavedSimSlot() == 1;
}

enum class AtJobKind : uint8_t {
    kDial,    // AT+CPIN? -> ATD<number>
    kHangup,  // ATH
};

enum class AtOutcome : uint8_t {
    kDialOk,         // ATD 收到 OK
    kSimNotReady,    // AT+CPIN? 没回 READY / 超时 / 模组不在
    kDialFailed,     // ATD 没回 OK（ERROR / NO CARRIER / 超时）
    kHangupDone,     // ATH 完成（成功与否都视作完成）
    kNo4G,           // 当前不是 4G 板（WiFi 模式）
};

struct AtJob {
    AtJobKind   kind;
    std::string number;   // only used for kDial
    uint32_t    epoch;    // 触发时记录的 s_call_epoch
};

struct AtResult {
    AtJobKind   kind;
    AtOutcome   outcome;
    uint32_t    epoch;
};

// 在 LVGL 线程里更新 UI（lv_async_call 的回调）。
void OnAtResult(void* user_data) {
    auto* res = static_cast<AtResult*>(user_data);
    // 屏幕已销毁，直接丢弃。
    if (!s_screen_active) {
        delete res;
        return;
    }
    // 状态已经变了（比如拨号还没回 OK 用户就按了挂断/返回），结果作废。
    if (res->epoch != s_call_epoch) {
        delete res;
        return;
    }

    if (res->kind == AtJobKind::kDial) {
        switch (res->outcome) {
            case AtOutcome::kDialOk:
                // ATD 已经收到 OK，正在通话中。
                SetStatusText(I18n::T("通话中"));
                break;
            case AtOutcome::kSimNotReady:
                s_call_state = CallState::kIdle;
                ++s_call_epoch;
                RefreshActionButton();
                RefreshNumberDisplay();
                SetStatusText(I18n::T("请检查移动网络"));
                break;
            case AtOutcome::kNo4G:
                s_call_state = CallState::kIdle;
                ++s_call_epoch;
                RefreshActionButton();
                RefreshNumberDisplay();
                SetStatusText(I18n::T("无 4G 模块"));
                break;
            case AtOutcome::kDialFailed:
            default:
                s_call_state = CallState::kIdle;
                ++s_call_epoch;
                RefreshActionButton();
                RefreshNumberDisplay();
                SetStatusText(I18n::T("拨号失败"));
                break;
        }
    } else {
        // 挂断的反馈不是必须展示的，简单清空状态行即可。
        if (s_call_state == CallState::kIdle) {
            SetStatusText("");
        }
    }
    delete res;
}

void AtJobTask(void* arg) {
    auto* job = static_cast<AtJob*>(arg);
    auto* result = new AtResult{};
    result->kind  = job->kind;
    result->epoch = job->epoch;

    auto* nt26 = GetNt26Board();
    if (nt26 == nullptr) {
        ESP_LOGW(TAG, "AT job kind=%d: 当前不在 4G 模式，跳过",
                 (int)job->kind);
        result->outcome = (job->kind == AtJobKind::kDial)
                              ? AtOutcome::kNo4G
                              : AtOutcome::kHangupDone;
    } else if (job->kind == AtJobKind::kHangup) {
        std::string resp;
        esp_err_t err = nt26->SendAtCommand("ATH", resp, 5000);
        ESP_LOGI(TAG, "AT 'ATH' -> err=%d resp='%s'", (int)err, resp.c_str());
        result->outcome = AtOutcome::kHangupDone;
    } else {
        // === Dial flow: AT+CPIN? -> ATD<number> ===
        std::string cpin_resp;
        esp_err_t cpin_err =
            nt26->SendAtCommand("AT+CPIN?", cpin_resp, 3000);
        ESP_LOGI(TAG, "AT 'AT+CPIN?' -> err=%d resp='%s'",
                 (int)cpin_err, cpin_resp.c_str());

        const bool sim_ready =
            (cpin_err == ESP_OK &&
             cpin_resp.find("READY") != std::string::npos &&
             cpin_resp.find("OK") != std::string::npos);

        if (!sim_ready) {
            result->outcome = AtOutcome::kSimNotReady;
        } else {
            std::string atd = "ATD";
            atd += job->number;
            std::string atd_resp;
            // ATD 通常 1-3s 回 OK；放宽到 10s 给模组留余量。
            esp_err_t err = nt26->SendAtCommand(atd, atd_resp, 10000);
            ESP_LOGI(TAG, "AT '%s' -> err=%d resp='%s'",
                     atd.c_str(), (int)err, atd_resp.c_str());
            result->outcome = (err == ESP_OK &&
                               atd_resp.find("OK") != std::string::npos)
                                  ? AtOutcome::kDialOk
                                  : AtOutcome::kDialFailed;
        }
    }

    lv_async_call(OnAtResult, result);
    delete job;
    vTaskDelete(nullptr);
}

void DispatchDial(const std::string& number) {
    auto* job = new AtJob{AtJobKind::kDial, number, s_call_epoch};
    BaseType_t r = xTaskCreate(AtJobTask, "call_at", 4096, job,
                               tskIDLE_PRIORITY + 2, nullptr);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(call_at dial) failed");
        delete job;
        SetStatusText(I18n::T("系统忙"));
    }
}

void DispatchHangup() {
    auto* job = new AtJob{AtJobKind::kHangup, "", s_call_epoch};
    BaseType_t r = xTaskCreate(AtJobTask, "call_at", 4096, job,
                               tskIDLE_PRIORITY + 2, nullptr);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(call_at hangup) failed");
        delete job;
    }
}

// ---------------------------------------------------------------------------
// State mutations
// ---------------------------------------------------------------------------

void AppendDigit(const char* d) {
    if (s_call_state == CallState::kCalling) return;
    size_t len = std::strlen(s_number);
    if (len >= kMaxDigits) return;
    s_number[len]     = d[0];
    s_number[len + 1] = '\0';
    RefreshNumberDisplay();
}

void Backspace() {
    if (s_call_state == CallState::kCalling) return;
    size_t len = std::strlen(s_number);
    if (len == 0) return;
    s_number[len - 1] = '\0';
    RefreshNumberDisplay();
}

void StartCall() {
    if (s_number[0] == '\0') return;

    // 内置卡（数据卡）不能拨号。保持 idle 状态，只在状态栏给出指引，
    // 让用户去 网络配置 → SIM 卡切换 把卡换成外置卡再拨。
    if (IsInternalSimActive()) {
        SetStatusText(I18n::T("内置卡无法拨打电话，请切换到外置卡"));
        return;
    }

    s_call_state = CallState::kCalling;
    ++s_call_epoch;
    RefreshActionButton();
    RefreshNumberDisplay();
    // 拨号前要先体检 SIM，UI 上先告诉用户在做什么，避免好像“按下没反应”。
    SetStatusText(I18n::T("正在检查网络..."));

    DispatchDial(s_number);
}

void HangupCall() {
    const bool was_calling = (s_call_state == CallState::kCalling);
    s_call_state = CallState::kIdle;
    ++s_call_epoch;
    RefreshActionButton();
    RefreshStatus();
    RefreshNumberDisplay();

    if (was_calling) {
        // 主动挂断: 通知模组释放当前通话。即便 ATD 还没回 OK，
        // 这条 ATH 也能被串行化进 modem 的 AT 队列（at_mutex_）。
        DispatchHangup();
    }
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

void KeyEventCb(lv_event_t* e) {
    const KeyDef* k = static_cast<const KeyDef*>(lv_event_get_user_data(e));
    if (k == nullptr) return;
    AppendDigit(k->digit);
}

void BackspaceEventCb(lv_event_t* e) {
    (void)e;
    Backspace();
}

// Long-press on backspace clears the whole number, matching iPhone behaviour.
void BackspaceLongPressCb(lv_event_t* e) {
    (void)e;
    if (s_call_state == CallState::kCalling) return;
    s_number[0] = '\0';
    RefreshNumberDisplay();
}

void ActionEventCb(lv_event_t* e) {
    (void)e;
    if (s_call_state == CallState::kIdle) {
        StartCall();
    } else {
        HangupCall();
    }
}

void OnSwipeBack() {
    // End an active call before leaving the dialer so the modem is not left
    // connected after the view is unloaded.
    if (s_call_state == CallState::kCalling) {
        DispatchHangup();
    }
    s_call_state = CallState::kIdle;
    ++s_call_epoch;
    s_screen_active = false;

    agent_ui::Navigation::Get().Back();
}

void OnScreenUnloaded(lv_event_t* e) {
    (void)e;
    // Mark the view inactive before asynchronous modem work can deliver a
    // callback to deleted LVGL objects.
    s_screen_active = false;
    s_number_lbl     = nullptr;
    s_status_lbl     = nullptr;
    s_backspace_btn  = nullptr;
    s_action_btn     = nullptr;
    s_action_icon    = nullptr;
    s_action_label   = nullptr;
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

void BuildNumberArea(lv_obj_t* parent) {
    const lv_font_t* number_font = fonts::Large();

    // Big number label, centered.  Sized to hug the font height so the
    // backspace button can vertically align with the actual text rather
    // than a tall padded box.
    s_number_lbl = lv_label_create(parent);
    lv_label_set_text(s_number_lbl, "");
    lv_obj_set_style_text_color(s_number_lbl,
                                lv_color_hex(Theme::Get().colors().text), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_number_lbl, number_font, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_number_lbl, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_number_lbl, LV_LABEL_LONG_SCROLL);
    lv_obj_set_size(s_number_lbl,
                    kPanelWidth - 2 * kPad,
                    number_font->line_height);
    lv_obj_set_pos(s_number_lbl, kPad, kNumberLblTop);
    lv_obj_remove_flag(s_number_lbl, LV_OBJ_FLAG_CLICKABLE);

    // Status line under the number ("拨号中...", etc.)
    s_status_lbl = lv_label_create(parent);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl,
                                lv_color_hex(Theme::Get().colors().muted), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_lbl, fonts::Small(), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);
    lv_obj_set_size(s_status_lbl, kPanelWidth - 2 * kPad, kStatusLblH);
    lv_obj_set_pos(s_status_lbl, kPad, kStatusLblTop);
    lv_obj_remove_flag(s_status_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* divider = lv_obj_create(parent);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, kPanelWidth, 1);
    lv_obj_set_pos(divider, 0, kNumberAreaH - 1);
    lv_obj_set_style_bg_color(divider,
                              lv_color_hex(Theme::Get().colors().border),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
}

void StyleKeyButton(lv_obj_t* btn) {
    const auto& colors = Theme::Get().colors();
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(colors.raised),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, metrics::kRadiusSmall, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
}

void BuildKeypad(lv_obj_t* parent) {
    // Match the browser demo's CSS grid exactly: 58 px horizontal padding,
    // 8 px vertical padding, three equal columns and four equal rows.
    const int grid_width = kPanelWidth - 2 * kKeypadSidePad;
    const int grid_height = kKeypadH - 2 * kKeypadVerticalPad;

    for (const auto& k : kKeys) {
        const int x1 = kKeypadSidePad + k.col * grid_width / kKeypadCols;
        const int x2 = kKeypadSidePad + (k.col + 1) * grid_width / kKeypadCols;
        const int y1 = kKeypadY + kKeypadVerticalPad +
                       k.row * grid_height / kDigitRows;
        const int y2 = kKeypadY + kKeypadVerticalPad +
                       (k.row + 1) * grid_height / kDigitRows;
        lv_obj_t* btn = ui_components::CreateButton(parent);
        lv_obj_set_size(btn, x2 - x1, y2 - y1);
        lv_obj_set_pos(btn, x1, y1);
        StyleKeyButton(btn);
        lv_obj_add_event_cb(btn, KeyEventCb, LV_EVENT_CLICKED,
                            const_cast<KeyDef*>(&k));

        // Inner column: digit on top, sub-letters below.
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t* digit = lv_label_create(btn);
        lv_label_set_text(digit, k.digit);
        lv_obj_set_style_text_color(digit, lv_color_hex(Theme::Get().colors().text), LV_PART_MAIN);
        lv_obj_set_style_text_font(digit, fonts::Large(), LV_PART_MAIN);
        lv_obj_remove_flag(digit, LV_OBJ_FLAG_CLICKABLE);

        if (k.sub != nullptr && k.sub[0] != '\0') {
            lv_obj_t* sub = lv_label_create(btn);
            lv_label_set_text(sub, k.sub);
            lv_obj_set_style_text_color(sub, lv_color_hex(Theme::Get().colors().muted),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(sub, fonts::Small(), LV_PART_MAIN);
            lv_obj_set_style_pad_top(sub, 2, LV_PART_MAIN);
            lv_obj_remove_flag(sub, LV_OBJ_FLAG_CLICKABLE);
        }
    }

}

void AddDeleteDigitIcon(lv_obj_t* button) {
    if (button == nullptr) return;
    static const lv_point_precise_t kOutline[] = {
        {9, 3}, {26, 3}, {26, 25}, {9, 25}, {2, 14}, {9, 3},
    };
    static const lv_point_precise_t kCrossA[] = {{13, 10}, {21, 18}};
    static const lv_point_precise_t kCrossB[] = {{21, 10}, {13, 18}};

    lv_obj_t* icon = lv_obj_create(button);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 28, 28);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    const auto add_line = [icon](const lv_point_precise_t* points,
                                 uint32_t point_count) {
        lv_obj_t* line = lv_line_create(icon);
        lv_line_set_points(line, points, point_count);
        lv_obj_set_style_line_color(
            line, lv_color_hex(Theme::Get().colors().muted), LV_PART_MAIN);
        lv_obj_set_style_line_width(line, 2, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
        lv_obj_set_pos(line, 0, 0);
    };
    add_line(kOutline, sizeof(kOutline) / sizeof(kOutline[0]));
    add_line(kCrossA, sizeof(kCrossA) / sizeof(kCrossA[0]));
    add_line(kCrossB, sizeof(kCrossB) / sizeof(kCrossB[0]));
}

void OnBackClicked(lv_event_t*) { OnSwipeBack(); }

}  // namespace

lv_obj_t* PhoneView::Create() {
    agent_ui::AppShell shell =
        agent_ui::CreateAppShell("电话", "移动网络", true, OnBackClicked);
    lv_obj_t* scr = shell.root;
    lv_obj_t* content = shell.content;

    s_number[0] = '\0';
    s_call_state = CallState::kIdle;
    ++s_call_epoch;
    s_screen_active = true;

    BuildNumberArea(content);
    BuildKeypad(content);

    auto action = ui_components::AddBottomPrimaryButton(
        shell.actions, FONT_AWESOME_PHONE, I18n::T("呼叫"), ActionEventCb);
    s_action_btn = action.root;
    s_action_icon = action.icon;
    s_action_label = action.label;
    auto remove = ui_components::AddBottomActionButton(
        shell.actions, "", I18n::T("删除"), BackspaceEventCb);
    s_backspace_btn = remove.root;
    AddDeleteDigitIcon(s_backspace_btn);
    lv_obj_add_event_cb(s_backspace_btn, BackspaceLongPressCb,
                        LV_EVENT_LONG_PRESSED, nullptr);

    RefreshNumberDisplay();
    RefreshActionButton();
    RefreshStatus();

    // Clear the active flag before asynchronous modem work can update this view.
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    AttachAppLifecycle(scr, PhoneView::LifecycleCallback);

    return scr;
}

void PhoneView::LifecycleCallback(AppLifecycleEvent event) {
    auto& io_expander = IOExpander::getInstance();
    if (event == AppLifecycleEvent::Load ||
        event == AppLifecycleEvent::Resume) {
        // 进入拨号界面：把功放切到 4G 通话路径。
        ESP_LOGI(TAG, "%s: PA_SWITCH=false (route to 4G)",
                 event == AppLifecycleEvent::Load ? "load" : "resume");
        io_expander.setLevel(IOExpander::Pin::PA_SWITCH, false);
        s_screen_active = true;
    } else {
        // 退出拨号界面：恢复默认 WIFI/本地音频路径。
        ESP_LOGI(TAG, "%s: PA_SWITCH=true (route to WIFI)",
                 event == AppLifecycleEvent::Unload ? "unload" : "suspend");
        io_expander.setLevel(IOExpander::Pin::PA_SWITCH, true);

        // Stop the modem call when a lifecycle path unloads the view directly.
        if (s_call_state == CallState::kCalling) {
            DispatchHangup();
            s_call_state = CallState::kIdle;
            ++s_call_epoch;
        }
        s_screen_active = false;
    }
}

}  // namespace agent_ui
