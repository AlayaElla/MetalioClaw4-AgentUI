#include "display_debug_view.h"

#include <array>
#include <cstdint>

#include <esp_log.h>

#include "board.h"
#include "core/app_shell.h"
#include "core/fonts.h"
#include "core/theme.h"
#include "core/ui_utils.h"
#include "display.h"

namespace agent_ui {
namespace {

constexpr char kTag[] = "DisplayDebug";
constexpr uint32_t kHardwarePatternDurationMs = 3000;

struct ColorSample {
    const char* name;
    uint32_t rgb;
    uint32_t text_rgb;
};

constexpr std::array<ColorSample, 6> kSamples = {{
    {"红  FF0000", 0xFF0000, 0xFFFFFF},
    {"绿  00FF00", 0x00FF00, 0x000000},
    {"蓝  0000FF", 0x0000FF, 0xFFFFFF},
    {"白  FFFFFF", 0xFFFFFF, 0x000000},
    {"灰  808080", 0x808080, 0xFFFFFF},
    {"黑  000000", 0x000000, 0xFFFFFF},
}};

struct UiState {
    lv_obj_t* root = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* solid_overlay = nullptr;
    lv_timer_t* pattern_timer = nullptr;
    bool hardware_pattern_active = false;
};

UiState s_ui;

Display* GetDisplay() {
    return Board::GetInstance().GetDisplay();
}

void SetStatus(const char* text, uint32_t color) {
    if (s_ui.status == nullptr || !lv_obj_is_valid(s_ui.status)) return;
    lv_label_set_text(s_ui.status, text != nullptr ? text : "");
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(color), LV_PART_MAIN);
}

void StopHardwarePattern(bool update_status) {
    if (s_ui.pattern_timer != nullptr) {
        lv_timer_delete(s_ui.pattern_timer);
        s_ui.pattern_timer = nullptr;
    }
    if (!s_ui.hardware_pattern_active) return;

    const bool stopped = GetDisplay() != nullptr &&
        GetDisplay()->SetDiagnosticPattern(DisplayDiagnosticPattern::None);
    s_ui.hardware_pattern_active = false;
    ESP_LOGI(kTag, "Hardware DSI pattern stopped: %s", stopped ? "ok" : "failed");
    if (update_status) {
        SetStatus(stopped ? "硬件色条结束；请与上方 LVGL 色块对比"
                          : "停止硬件色条失败，请检查 Monitor",
                  stopped ? 0x5F6B7A : 0xD14343);
    }
}

void OnPatternTimeout(lv_timer_t*) {
    // LVGL deletes a timer automatically after its final repeat. Clear our
    // handle first so StopHardwarePattern does not delete the active callback.
    s_ui.pattern_timer = nullptr;
    StopHardwarePattern(true);
}

void StartHardwarePattern(DisplayDiagnosticPattern pattern) {
    StopHardwarePattern(false);
    Display* display = GetDisplay();
    if (display == nullptr || !display->SetDiagnosticPattern(pattern)) {
        ESP_LOGE(kTag, "Failed to start hardware DSI pattern=%d",
                 static_cast<int>(pattern));
        SetStatus("硬件色条启动失败，请检查 Monitor", 0xD14343);
        return;
    }

    s_ui.hardware_pattern_active = true;
    s_ui.pattern_timer = lv_timer_create(OnPatternTimeout,
                                         kHardwarePatternDurationMs, nullptr);
    lv_timer_set_repeat_count(s_ui.pattern_timer, 1);
    SetStatus("正在显示硬件 DSI 色条，3 秒后自动返回", 0x2563EB);
    ESP_LOGI(kTag, "Hardware DSI pattern=%d started for %lu ms",
             static_cast<int>(pattern),
             static_cast<unsigned long>(kHardwarePatternDurationMs));
}

void OnPatternClicked(lv_event_t* event) {
    const auto pattern = static_cast<DisplayDiagnosticPattern>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    StartHardwarePattern(pattern);
}

void CloseSolidOverlay(lv_event_t*) {
    if (s_ui.solid_overlay == nullptr) return;
    lv_obj_delete(s_ui.solid_overlay);
    s_ui.solid_overlay = nullptr;
    SetStatus("全屏色块结束", 0x5F6B7A);
}

void OnSampleClicked(lv_event_t* event) {
    const auto* sample = static_cast<const ColorSample*>(
        lv_event_get_user_data(event));
    if (sample == nullptr || s_ui.root == nullptr) return;

    if (s_ui.solid_overlay != nullptr) {
        lv_obj_delete(s_ui.solid_overlay);
    }
    lv_obj_t* overlay = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, metrics::kDisplaySize, metrics::kDisplaySize);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(sample->rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, CloseSolidOverlay, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_foreground(overlay);
    s_ui.solid_overlay = overlay;
    ESP_LOGI(kTag, "LVGL fullscreen sample: %s", sample->name);
}

lv_obj_t* CreatePatternButton(lv_obj_t* parent, const char* text,
                              DisplayDiagnosticPattern pattern) {
    lv_obj_t* button = lv_button_create(parent);
    StyleButton(button, false);
    lv_obj_set_size(button, 300, 62);
    lv_obj_add_event_cb(button, OnPatternClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(pattern)));

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

void OnDeleted(lv_event_t* event) {
    if (lv_event_get_target_obj(event) != s_ui.root) return;
    StopHardwarePattern(false);
    s_ui = {};
}

void LifecycleCallback(AppLifecycleEvent event) {
    if (event == AppLifecycleEvent::Suspend || event == AppLifecycleEvent::Unload) {
        StopHardwarePattern(false);
    }
}

}  // namespace

lv_obj_t* DisplayDebugView::Create() {
    auto shell = CreateAppShell("显示调试", nullptr);
    s_ui = {};
    s_ui.root = shell.root;
    lv_obj_add_event_cb(shell.root, OnDeleted, LV_EVENT_DELETE, nullptr);
    AttachAppLifecycle(shell.root, LifecycleCallback);

    const auto& colors = Theme::Get().colors();

    lv_obj_t* title = lv_label_create(shell.content);
    lv_label_set_text(title, "显示调试 · LVGL 参考色");
    lv_obj_set_style_text_font(title, fonts::MediumBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(title, 28, 18);

    lv_obj_t* hint = lv_label_create(shell.content);
    lv_label_set_text(hint, "点击色块可全屏查看；再次点击全屏退出");
    lv_obj_set_style_text_font(hint, fonts::Small(), LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(colors.muted), LV_PART_MAIN);
    lv_obj_set_pos(hint, 330, 22);

    for (size_t i = 0; i < kSamples.size(); ++i) {
        const int column = static_cast<int>(i % 3);
        const int row = static_cast<int>(i / 3);
        lv_obj_t* sample = lv_button_create(shell.content);
        lv_obj_remove_style_all(sample);
        lv_obj_set_size(sample, 204, 82);
        lv_obj_set_pos(sample, 28 + column * 220, 66 + row * 96);
        lv_obj_set_style_bg_color(sample, lv_color_hex(kSamples[i].rgb), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sample, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(sample, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(sample, lv_color_hex(colors.border), LV_PART_MAIN);
        lv_obj_set_style_radius(sample, 12, LV_PART_MAIN);
        lv_obj_add_event_cb(sample, OnSampleClicked, LV_EVENT_CLICKED,
                            const_cast<ColorSample*>(&kSamples[i]));

        lv_obj_t* label = lv_label_create(sample);
        lv_label_set_text(label, kSamples[i].name);
        lv_obj_set_style_text_font(label, fonts::SmallBold(), LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(kSamples[i].text_rgb),
                                    LV_PART_MAIN);
        lv_obj_center(label);
    }

    lv_obj_t* divider = lv_obj_create(shell.content);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 664, 1);
    lv_obj_set_pos(divider, 28, 270);
    lv_obj_set_style_bg_color(divider, lv_color_hex(colors.border), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* hardware_title = lv_label_create(shell.content);
    lv_label_set_text(hardware_title, "DSI Host 硬件图案（绕过 LVGL / PPA）");
    lv_obj_set_style_text_font(hardware_title, fonts::SmallBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(hardware_title, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_pos(hardware_title, 28, 290);

    lv_obj_t* vertical = CreatePatternButton(
        shell.content, "竖向色条 · 3 秒",
        DisplayDiagnosticPattern::ColorBarsVertical);
    lv_obj_set_pos(vertical, 28, 330);

    lv_obj_t* horizontal = CreatePatternButton(
        shell.content, "横向色条 · 3 秒",
        DisplayDiagnosticPattern::ColorBarsHorizontal);
    lv_obj_set_pos(horizontal, 356, 330);

    s_ui.status = lv_label_create(shell.content);
    lv_label_set_text(s_ui.status,
                      "对比白色与灰色：两条路径都偏绿说明在 DSI/面板侧");
    lv_obj_set_style_text_font(s_ui.status, fonts::Small(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(colors.muted), LV_PART_MAIN);
    lv_obj_set_pos(s_ui.status, 28, 414);

    ESP_LOGI(kTag, "Display debug app opened: LVGL reference colors and DSI patterns ready");
    return shell.root;
}

}  // namespace agent_ui
