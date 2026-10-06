#include "metalio_app_api.h"
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {
struct Recorder {
    const metalio_app_host_api_t* api = nullptr;
    void* host = nullptr;
    metalio_app_theme_t theme{};
    metalio_app_widget_t title{}, clock{}, message{}, detail{}, level{}, level_text{};
    metalio_app_widget_t record{}, replay{}, discard{}, upper_line{}, lower_line{};
    metalio_app_widget_t waveform[12]{};
    uint8_t peaks[12]{};
    metalio_app_recording_status_t status{};
    char saved_path[METALIO_APP_RECORDING_PATH_BYTES]{};
    uint32_t wall_ms = 0;
    bool recording = false;
    bool stopping = false;
    bool playing = false;
    bool starting = false;
    bool playback_failed = false;
    uint32_t playback_wait_ms = 0;
} s;

#define HAS_API(name) (s.api->struct_size >= offsetof(metalio_app_host_api_t, name) + sizeof(s.api->name) && s.api->name != nullptr)

void Message(const char* text) { s.api->set_label_text(s.host, s.message, text); }
void Buttons() {
    s.api->set_button_text(s.host, s.record, s.recording ? "保存" : "录音");
    s.api->set_button_enabled(s.host, s.record, !s.stopping && !s.playing);
    s.api->set_button_text(s.host, s.replay, s.playing ? "停止" : "回放");
    s.api->set_button_enabled(s.host, s.replay,
        !s.recording && !s.stopping && s.saved_path[0] != '\0');
    s.api->set_button_enabled(s.host, s.discard, s.recording && !s.stopping);
}

const char* ErrorText(int error) {
    switch (error) {
        case METALIO_APP_RECORDING_ERROR_BUSY: return "麦克风正在使用，请稍后重试";
        case METALIO_APP_RECORDING_ERROR_STORAGE: return "无法保存，请检查 SD 卡";
        case METALIO_APP_RECORDING_ERROR_AUDIO: return "录音启动失败";
        case METALIO_APP_RECORDING_ERROR_WRITE: return "保存失败，请检查 SD 卡";
        default: return "录音失败，请重试";
    }
}

void Record(void*) {
    if (s.stopping || s.playing) return;
    if (s.recording) {
        if (s.api->recording_stop(s.host) == 0) {
            s.stopping = true;
            Message("正在保存…");
        } else Message("停止录音失败，请重试");
        Buttons();
        return;
    }
    const metalio_app_recording_config_t config{"Voice", 300000};
    const int result = s.api->recording_start(s.host, &config);
    if (result != 0) { Message(ErrorText(result)); return; }
    s.wall_ms = 0;
    s.recording = true;
    s.stopping = false;
    s.status = {};
    for (auto& peak : s.peaks) peak = 0;
    Message("正在录音，请对着设备说话");
    s.api->set_bar_value(s.host, s.level, 0);
    Buttons();
}

void Discard(void*) {
    if (!s.recording || s.stopping) return;
    if (s.api->recording_cancel(s.host) == 0) {
        s.stopping = true;
        Message("正在取消…");
    }
    Buttons();
}

void Replay(void*) {
    if (s.recording || s.stopping || s.saved_path[0] == '\0') return;
    if (s.playing) {
        s.api->media_stop(s.host);
        s.playing = false;
        s.starting = false;
        Message("回放已停止");
    } else {
        if (s.api->media_start(s.host, s.saved_path) != 0) {
            Message("回放启动失败，请检查系统版本");
            return;
        }
        s.playing = true;
        s.starting = true;
        s.playback_failed = false;
        s.playback_wait_ms = 0;
        Message("正在打开录音…");
    }
    Buttons();
}

void Refresh(void*) {
    metalio_app_recording_status_t status{};
    if (s.api->recording_get_status(s.host, &status) != 0) return;
    const bool was_active = s.recording || s.stopping;
    if (s.recording && !s.stopping) s.wall_ms += 100;
    s.status = status;
    char text[220];
    const uint32_t elapsed = was_active ? s.wall_ms : status.duration_ms;
    std::snprintf(text, sizeof(text), "%02u:%02u.%u", unsigned(elapsed / 60000),
        unsigned((elapsed / 1000) % 60), unsigned((elapsed / 100) % 10));
    s.api->set_label_text(s.host, s.clock, text);
    s.api->set_bar_value(s.host, s.level, s.recording ? status.peak_percent * 10U : 0);
    std::snprintf(text, sizeof(text), "麦克风电平  %u%%", s.recording ? status.peak_percent : 0U);
    s.api->set_label_text(s.host, s.level_text, text);
    for (unsigned i = 0; i < 11; ++i) s.peaks[i] = s.peaks[i + 1];
    s.peaks[11] = s.recording ? status.peak_percent : 0;
    for (unsigned i = 0; i < 12; ++i) {
        const int16_t height = static_cast<int16_t>(10 + s.peaks[i] * 80U / 100);
        s.api->set_widget_bounds(s.host, s.waveform[i], static_cast<int16_t>(60 + i * 51),
            static_cast<int16_t>(338 - height / 2), 37, height);
    }
    std::snprintf(text, sizeof(text), "音频 %u KB   ·   丢帧 %u",
        unsigned(status.data_bytes / 1024), unsigned(status.dropped_frames));
    s.api->set_label_text(s.host, s.detail, text);
    if (s.recording && !s.stopping && status.state == METALIO_APP_RECORDING_RECORDING) {
        if (s.wall_ms >= 1500 && status.data_bytes == 0)
            Message("尚未收到麦克风音频");
        else if (status.data_bytes != 0) Message("正在录音，请对着设备说话");
        if (s.wall_ms >= 300000) Record(nullptr);
    }
    if (was_active && status.state == METALIO_APP_RECORDING_COMPLETED) {
        s.recording = false;
        s.stopping = false;
        if (status.data_bytes == 0) Message("没有收到音频，请重新录音");
        else {
            std::snprintf(s.saved_path, sizeof(s.saved_path), "%s", status.path);
            const char* name = status.path;
            for (const char* cursor = status.path; *cursor != '\0'; ++cursor)
                if (*cursor == '/') name = cursor + 1;
            std::snprintf(text, sizeof(text), "已保存：%s", name);
            Message(text);
        }
        Buttons();
    } else if (was_active && (status.state == METALIO_APP_RECORDING_CANCELLED ||
                              status.state == METALIO_APP_RECORDING_ERROR)) {
        s.recording = false;
        s.stopping = false;
        Message(status.state == METALIO_APP_RECORDING_CANCELLED ? "本次录音已取消" : ErrorText(status.error));
        Buttons();
    }
    if (s.playing) {
        s.playback_wait_ms += 100;
        metalio_app_media_state_t state{};
        if (s.api->media_get_state(s.host, &state) == 0) {
            if (state == METALIO_APP_MEDIA_PLAYING) {
                s.starting = false;
                Message("正在回放录音");
            } else if (state == METALIO_APP_MEDIA_ERROR) {
                s.playback_failed = true;
                Message("回放失败，请检查录音文件");
            } else if (state == METALIO_APP_MEDIA_IDLE && (!s.starting || s.playback_wait_ms >= 500)) {
                s.playing = false;
                if (!s.playback_failed) Message(s.starting ? "回放已结束" : "回放完成");
                Buttons();
            }
            if (s.playback_failed) { s.starting = false; s.api->media_stop(s.host); }
        }
    }
}

void ApplyTheme(const metalio_app_theme_t* theme) {
    if (theme == nullptr) return;
    s.theme = *theme;
    s.api->set_background(s.host, theme->background);
    s.api->set_rect_color(s.host, s.upper_line, theme->border);
    s.api->set_rect_color(s.host, s.lower_line, theme->border);
    for (auto bar : s.waveform) s.api->set_rect_color(s.host, bar, theme->accent);
    s.api->set_label_color(s.host, s.title, theme->text);
    s.api->set_label_color(s.host, s.clock, theme->text);
    s.api->set_label_color(s.host, s.message, theme->text);
    s.api->set_label_color(s.host, s.detail, theme->muted);
    s.api->set_label_color(s.host, s.level_text, theme->muted);
    s.api->set_bar_colors(s.host, s.level, theme->border, theme->accent);
    s.api->set_button_colors(s.host, s.record, theme->accent, theme->accent_pressed, theme->accent_ink);
    s.api->set_button_border(s.host, s.record, theme->accent, 1, 14);
    const metalio_app_widget_t secondary_buttons[] = {s.replay, s.discard};
    for (auto button : secondary_buttons) {
        s.api->set_button_colors(s.host, button, theme->background, theme->raised, theme->text);
        s.api->set_button_border(s.host, button, theme->border, 1, 14);
    }
}
void ThemeChanged(void*, const metalio_app_theme_t* theme) { ApplyTheme(theme); }

bool Build() {
    auto label = [](metalio_app_widget_t* widget, const char* text, int16_t y, int16_t height, metalio_app_font_t font) {
        return s.api->add_label_ex(s.host, text, 54, y, 612, height, s.theme.text, font, widget) == 0 &&
               s.api->set_label_alignment(s.host, *widget, METALIO_APP_TEXT_ALIGN_CENTER) == 0;
    };
    for (unsigned i = 0; i < 12; ++i)
        if (s.api->add_rect(s.host, static_cast<int16_t>(60 + i * 51), 333, 37, 10,
            s.theme.accent, 5, &s.waveform[i]) != 0) return false;
    return label(&s.title, "录音机", 40, 70, METALIO_APP_FONT_LARGE_BOLD) &&
        label(&s.clock, "00:00.0", 142, 60, METALIO_APP_FONT_LARGE_BOLD) &&
        label(&s.message, "点击录音开始", 390, 30, METALIO_APP_FONT_SMALL) &&
        s.api->add_rect(s.host, 50, 424, 620, 1, s.theme.border, 0, &s.upper_line) == 0 &&
        s.api->add_rect(s.host, 50, 503, 620, 1, s.theme.border, 0, &s.lower_line) == 0 &&
        s.api->add_label_ex(s.host, "麦克风电平  0%", 50, 438, 250, 28,
            s.theme.muted, METALIO_APP_FONT_SMALL, &s.level_text) == 0 &&
        s.api->add_bar(s.host, 316, 445, 354, 12, s.theme.border, s.theme.accent, &s.level) == 0 &&
        label(&s.detail, "音频 0 KB   ·   丢帧 0", 471, 26, METALIO_APP_FONT_SMALL) &&
        s.api->add_button(s.host, "录音", 80, 214, 176, 72, s.theme.accent, s.theme.accent_ink,
            METALIO_APP_FONT_MEDIUM_BOLD, Record, nullptr, &s.record) == 0 &&
        s.api->add_button(s.host, "回放", 272, 214, 176, 72, s.theme.background, s.theme.text,
            METALIO_APP_FONT_MEDIUM_BOLD, Replay, nullptr, &s.replay) == 0 &&
        s.api->add_button(s.host, "取消", 464, 214, 176, 72, s.theme.background, s.theme.text,
            METALIO_APP_FONT_MEDIUM_BOLD, Discard, nullptr, &s.discard) == 0;
}
}  // namespace

extern "C" int main(int argc, char* argv[]) {
    if (argc != 2 || argv == nullptr || argv[0] == nullptr || argv[1] == nullptr) return 1;
    s = {};
    s.api = reinterpret_cast<const metalio_app_host_api_t*>(argv[0]);
    const auto* launch = reinterpret_cast<const metalio_app_launch_context_t*>(argv[1]);
    if (s.api->abi_version != METALIO_APP_ABI_VERSION || launch->abi_version != METALIO_APP_ABI_VERSION ||
        launch->struct_size < sizeof(*launch) || launch->content_width < 720 || launch->content_height < 526) return 2;
    s.host = launch->host_context;
    if (!HAS_API(set_label_alignment) || !HAS_API(set_theme_callback) || !HAS_API(set_button_colors) ||
        !HAS_API(recording_start) || !HAS_API(recording_stop) || !HAS_API(recording_cancel) ||
        !HAS_API(recording_get_status) || !HAS_API(media_start) || !HAS_API(media_stop) ||
        !HAS_API(media_get_state) || !HAS_API(get_capabilities) || !HAS_API(get_theme) ||
        !HAS_API(set_button_border) || !HAS_API(set_widget_bounds) ||
        !HAS_API(add_label_ex) || !HAS_API(set_label_text) || !HAS_API(set_interval) ||
        !HAS_API(add_rect) || !HAS_API(set_rect_color) || !HAS_API(set_label_color) ||
        !HAS_API(add_bar) || !HAS_API(set_bar_value) || !HAS_API(set_bar_colors) ||
        !HAS_API(add_button) || !HAS_API(set_button_text) || !HAS_API(set_button_enabled)) return 3;
    const auto caps = s.api->get_capabilities(s.host);
    const auto required = METALIO_APP_CAP_AUDIO_RECORDING | METALIO_APP_CAP_UI_THEME |
        METALIO_APP_CAP_UI_BUTTONS | METALIO_APP_CAP_UI_DRAW | METALIO_APP_CAP_UI_CONTROLS;
    if ((caps & required) != required || s.api->get_theme(s.host, &s.theme) != 0) return 4;
    if (!Build()) return 5;
    Buttons();
    ApplyTheme(&s.theme);
    if (s.api->set_theme_callback(s.host, ThemeChanged, nullptr) != 0 ||
        s.api->set_interval(s.host, 100, Refresh, nullptr) != 0) return 6;
    return 0;
}
