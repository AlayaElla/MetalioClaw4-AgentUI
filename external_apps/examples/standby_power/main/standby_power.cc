#include "metalio_app_api.h"
#include <cstddef>
#include <cstdio>
#include <initializer_list>

namespace {
struct State {
    const metalio_app_host_api_t* api = nullptr;
    void* host = nullptr;
    metalio_app_theme_t theme{};
    metalio_app_widget_t title{}, frequency{}, policy{}, battery{}, power{}, note{}, status{}, line{};
    metalio_app_widget_t manual{}, short_test{}, long_test{};
    bool timed = false;
} s;

#define HAS_API(name) (s.api->struct_size >= offsetof(metalio_app_host_api_t, name) + sizeof(s.api->name) && s.api->name != nullptr)

void Message(const char* text) { s.api->set_label_text(s.host, s.status, text); }
void EnableButtons(bool enabled) {
    for (auto button : {s.manual, s.short_test, s.long_test})
        s.api->set_button_enabled(s.host, button, enabled);
}

void Start(uint32_t duration_ms) {
    s.timed = duration_ms != 0;
    Message(s.timed ? "确认黑屏后测量，完成后恢复锁屏" : "已请求手动待机，请使用侧键唤醒");
    EnableButtons(false);
    const int result = s.api->standby_start(s.host, duration_ms);
    if (result != METALIO_APP_POWER_OK) {
        s.timed = false;
        EnableButtons(true);
        Message(result == METALIO_APP_POWER_ERROR_MEMORY ? "测试资源不足，请稍后重试" :
                result == METALIO_APP_POWER_ERROR_BUSY ? "已有测试运行，请等待结束" :
                                                       "无法进入待机，请重试");
    } else if (!s.timed) EnableButtons(true);
}
void Manual(void*) { Start(0); }
void ShortTest(void*) { Start(15000); }
void LongTest(void*) { Start(60000); }

void Refresh(void*) {
    char text[448];
    metalio_app_power_reading_t reading{};
    if (s.api->get_power_reading(s.host, &reading) == 0) {
        std::snprintf(text, sizeof(text), "当前频率：%lu MHz",
                      static_cast<unsigned long>(reading.cpu_frequency_mhz));
        s.api->set_label_text(s.host, s.frequency, text);
        std::snprintf(text, sizeof(text), "应用上限：%ld MHz   黑屏目标：%ld MHz",
                      static_cast<long>(reading.applied_max_mhz), static_cast<long>(reading.screen_off_max_mhz));
        s.api->set_label_text(s.host, s.policy, text);
        if (reading.battery_valid)
            std::snprintf(text, sizeof(text), "电量：%d%%   充电：%s", reading.battery_percent, reading.charging ? "是" : "否");
        else std::snprintf(text, sizeof(text), "电量：暂不可用");
        s.api->set_label_text(s.host, s.battery, text);
        if (reading.power_valid)
            std::snprintf(text, sizeof(text), "电池：%ld mV   电流：%+ld mA（正充 / 负放）",
                          static_cast<long>(reading.voltage_mv), static_cast<long>(reading.current_ma));
        else std::snprintf(text, sizeof(text), "电池电压 / 电流：暂不可用");
        s.api->set_label_text(s.host, s.power, text);
    }
    if (!s.timed) return;
    metalio_app_standby_result_t result{};
    if (s.api->standby_get_result(s.host, &result) != 0) {
        Message("测试结果暂不可用");
        return;
    }
    const bool busy = result.state == METALIO_APP_STANDBY_WAITING ||
                      result.state == METALIO_APP_STANDBY_MEASURING ||
                      result.state == METALIO_APP_STANDBY_WAKING;
    EnableButtons(!busy);
    if (busy) {
        Message(result.state == METALIO_APP_STANDBY_WAITING ? "等待屏幕停止，准备测量" :
                result.state == METALIO_APP_STANDBY_MEASURING ? "正在采集黑屏功耗" : "正在恢复待机锁屏");
        return;
    }
    if (result.state == METALIO_APP_STANDBY_ERROR) {
        Message(result.error == METALIO_APP_POWER_ERROR_WAKE ? "自动唤醒失败，请按侧键恢复后重试" :
                                                              "未确认屏幕停止，没有完成黑屏测量");
        return;
    }
    if (result.state == METALIO_APP_STANDBY_IDLE) return;
    char current[96];
    if (result.sample_count)
        std::snprintf(current, sizeof(current), "平均电流：%+ld mA（%lu/%lu次）%s",
                      static_cast<long>(result.average_current_ma), static_cast<unsigned long>(result.sample_count),
                      static_cast<unsigned long>(result.sample_attempts), result.external_power ? "；外接电源" : "");
    else std::snprintf(current, sizeof(current), "黑屏电流未读到，请重试");
    char frequency[80];
    if (result.black_screen_mhz)
        std::snprintf(frequency, sizeof(frequency), "实测 %lu MHz（上限 %ld MHz）",
                      static_cast<unsigned long>(result.black_screen_mhz), static_cast<long>(result.applied_max_mhz));
    else std::snprintf(frequency, sizeof(frequency), "黑屏频率未采集");
    std::snprintf(text, sizeof(text), "%s；黑屏 %lu ms\n%s\n%s\n轻睡眠 %lu ms（%lu次）",
                  result.state == METALIO_APP_STANDBY_CANCELLED ? "侧键提前唤醒，测试已取消" :
                      result.woke_to_lock_screen ? "已恢复待机锁屏" : "测量结束",
                  static_cast<unsigned long>(result.elapsed_ms), frequency, current, static_cast<unsigned long>(result.sleep_ms),
                  static_cast<unsigned long>(result.sleep_entries));
    Message(text);
}

void ApplyTheme(const metalio_app_theme_t* theme) {
    if (!theme) return;
    s.theme = *theme;
    s.api->set_background(s.host, theme->background);
    s.api->set_rect_color(s.host, s.line, theme->border);
    for (auto label : {s.title, s.frequency, s.status}) s.api->set_label_color(s.host, label, theme->text);
    for (auto label : {s.policy, s.battery, s.power, s.note}) s.api->set_label_color(s.host, label, theme->muted);
    for (auto button : {s.manual, s.short_test, s.long_test}) {
        s.api->set_button_colors(s.host, button, theme->background, theme->raised, theme->text);
        s.api->set_button_border(s.host, button, theme->border, 1, 14);
    }
}
void ThemeChanged(void*, const metalio_app_theme_t* theme) { ApplyTheme(theme); }

bool Build() {
    const auto label = [](metalio_app_widget_t* widget, const char* text, int16_t y, int16_t height,
                          metalio_app_font_t font = METALIO_APP_FONT_MEDIUM) {
        return s.api->add_label_ex(s.host, text, 28, y, 664, height, s.theme.text, font, widget) == 0;
    };
    return label(&s.title, "待机功耗", 20, 66, METALIO_APP_FONT_LARGE_BOLD) &&
        label(&s.frequency, "当前频率：读取中", 90, 40) &&
        label(&s.policy, "应用上限 / 黑屏目标：读取中", 140, 36, METALIO_APP_FONT_SMALL) &&
        label(&s.battery, "电量：读取中", 182, 36, METALIO_APP_FONT_SMALL) &&
        label(&s.power, "电池电压 / 电流：读取中", 224, 36, METALIO_APP_FONT_SMALL) &&
        label(&s.note, "15 / 60 秒测试测量联网黑屏阶段的电流", 266, 32, METALIO_APP_FONT_SMALL) &&
        s.api->add_rect(s.host, 28, 308, 664, 1, s.theme.border, 0, &s.line) == 0 &&
        label(&s.status, "选择测试方式，解锁后查看结果", 322, 120, METALIO_APP_FONT_SMALL) &&
        s.api->add_button(s.host, "手动待机", 28, 452, 204, 64, s.theme.background, s.theme.text,
                          METALIO_APP_FONT_MEDIUM, Manual, nullptr, &s.manual) == 0 &&
        s.api->add_button(s.host, "15 秒唤醒", 242, 452, 218, 64, s.theme.background, s.theme.text,
                          METALIO_APP_FONT_MEDIUM, ShortTest, nullptr, &s.short_test) == 0 &&
        s.api->add_button(s.host, "60 秒唤醒", 470, 452, 222, 64, s.theme.background, s.theme.text,
                          METALIO_APP_FONT_MEDIUM, LongTest, nullptr, &s.long_test) == 0;
}
}  // namespace

extern "C" int main(const metalio_app_host_api_t* api, const metalio_app_launch_context_t* context) {
    if (!api || !context || api->abi_version != METALIO_APP_ABI_VERSION ||
        context->abi_version != METALIO_APP_ABI_VERSION || context->struct_size < sizeof(*context) ||
        !context->host_context || context->content_width < 720 || context->content_height < 526) return -1;
    s = {};
    s.api = api;
    s.host = context->host_context;
    if (!HAS_API(standby_cancel) || !HAS_API(standby_get_result) || !HAS_API(standby_start) ||
        !HAS_API(get_power_reading) || !HAS_API(get_capabilities) || !HAS_API(set_background) ||
        !HAS_API(add_label_ex) || !HAS_API(set_label_text) || !HAS_API(set_interval) ||
        !HAS_API(add_rect) || !HAS_API(set_rect_color) || !HAS_API(set_label_color) ||
        !HAS_API(add_button) || !HAS_API(set_button_enabled) || !HAS_API(set_button_colors) ||
        !HAS_API(set_button_border) || !HAS_API(get_theme) || !HAS_API(set_theme_callback)) return -1;
    const uint64_t required = METALIO_APP_CAP_POWER_DIAGNOSTICS | METALIO_APP_CAP_UI_THEME |
                              METALIO_APP_CAP_UI_BUTTONS | METALIO_APP_CAP_UI_DRAW | METALIO_APP_CAP_UI_CONTROLS;
    if ((api->get_capabilities(s.host) & required) != required || api->get_theme(s.host, &s.theme) != 0) return -1;
    if (!Build()) return -1;
    ApplyTheme(&s.theme);
    Refresh(nullptr);
    if (api->set_theme_callback(s.host, ThemeChanged, nullptr) != 0) return -1;
    return api->set_interval(s.host, 500, Refresh, nullptr);
}

extern "C" void metalio_app_stop(void*) {
    if (s.api && HAS_API(standby_cancel)) s.api->standby_cancel(s.host);
    s = {};
}
