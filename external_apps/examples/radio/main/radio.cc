#include "metalio_app_api.h"

#include <cstddef>
#include <cstdint>

namespace {

constexpr uint32_t kMaximumStations = 48;
constexpr uint32_t kConfigurationCapacity = 8192;
constexpr uint32_t kSavedStationUrlCapacity = 1024;
constexpr const char* kSavedStationPath = "last-station-url.txt";
constexpr int16_t kSpectrumBottom = 410;
constexpr int16_t kSpectrumMaximumHeight = 220;
constexpr int16_t kSpectrumBarWidth = 38;
constexpr int16_t kSpectrumGap = 13;

size_t TextLength(const char* text) {
    size_t length = 0;
    if (text == nullptr) return 0;
    while (text[length] != '\0') ++length;
    return length;
}

bool TextEquals(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) return left == right;
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

bool TextStartsWith(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) return false;
    while (*prefix != '\0') {
        if (*text++ != *prefix++) return false;
    }
    return true;
}

void FormatPercent(char* output, size_t capacity, uint8_t value) {
    if (output == nullptr || capacity == 0) return;
    char reversed[3]{};
    size_t digits = 0;
    unsigned remaining = value;
    do {
        reversed[digits++] = static_cast<char>('0' + remaining % 10U);
        remaining /= 10U;
    } while (remaining != 0 && digits < sizeof(reversed));

    size_t written = 0;
    while (digits != 0 && written + 1 < capacity) {
        output[written++] = reversed[--digits];
    }
    if (written + 1 < capacity) output[written++] = '%';
    output[written] = '\0';
}

struct RadioStation {
    const char* name = nullptr;
    const char* url = nullptr;
};

struct AppState {
    const metalio_app_host_api_t* api = nullptr;
    const metalio_app_launch_context_t* launch = nullptr;
    metalio_app_capabilities_t capabilities = 0;
    metalio_app_theme_t theme{};
    char station_strings[kConfigurationCapacity + 1]{};
    RadioStation stations[kMaximumStations]{};
    const char* station_labels[kMaximumStations]{};
    uint32_t station_count = 0;
    uint32_t requested_station = 0;
    uint32_t playing_station = 0;
    uint32_t saved_station = kMaximumStations;
    metalio_app_media_state_t media_state = METALIO_APP_MEDIA_IDLE;
    bool tune_pending = false;
    metalio_app_widget_t station_label = 0;
    metalio_app_widget_t spectrum[METALIO_APP_MEDIA_SPECTRUM_BANDS]{};
    metalio_app_widget_t upper_divider = 0;
    metalio_app_widget_t lower_divider = 0;
    metalio_app_widget_t volume_icon = 0;
    metalio_app_widget_t volume_label = 0;
    metalio_app_widget_t volume_slider = 0;
    metalio_app_widget_t station_picker = 0;
    uint8_t volume = 50;
};

AppState s_app{};
uint8_t s_configuration[kConfigurationCapacity + 1]{};
uint8_t s_saved_station_url[kSavedStationUrlCapacity + 1]{};

class JsonReader {
public:
    JsonReader(const char* begin, size_t length, char* string_pool,
               size_t string_pool_capacity)
        : cursor_(begin), end_(begin + length), string_pool_(string_pool),
          string_pool_capacity_(string_pool_capacity) {}

    bool Parse(RadioStation* stations, uint32_t capacity,
               uint32_t* station_count, uint32_t* default_station) {
        if (stations == nullptr || station_count == nullptr || default_station == nullptr || !Take('{')) return false;
        bool have_stations = false;
        bool first = true;
        while (true) {
            SkipWhitespace();
            if (Take('}')) break;
            if (!first && !Take(',')) return false;
            first = false;
            char key[24]{};
            if (!ReadString(key, sizeof(key)) || !Take(':')) return false;
            if (TextEquals(key, "default")) {
                uint32_t value = 0;
                if (!ReadUnsigned(&value)) return false;
                *default_station = value;
            } else if (TextEquals(key, "stations")) {
                if (!ReadStationArray(stations, capacity, station_count)) return false;
                have_stations = true;
            } else if (!SkipValue()) {
                return false;
            }
        }
        SkipWhitespace();
        return cursor_ == end_ && have_stations && *station_count != 0 &&
            *default_station >= 1 && *default_station <= *station_count;
    }

private:
    void SkipWhitespace() {
        while (cursor_ < end_ && (*cursor_ == ' ' || *cursor_ == '\t' ||
               *cursor_ == '\r' || *cursor_ == '\n')) ++cursor_;
    }

    bool Take(char expected) {
        SkipWhitespace();
        if (cursor_ >= end_ || *cursor_ != expected) return false;
        ++cursor_;
        return true;
    }

    static bool AppendByte(char* output, size_t capacity, size_t* length, char value) {
        if (*length + 1 >= capacity) return false;
        output[(*length)++] = value;
        output[*length] = '\0';
        return true;
    }

    static bool AppendCodePoint(char* output, size_t capacity, size_t* length, uint32_t value) {
        if (value <= 0x7f) return AppendByte(output, capacity, length, static_cast<char>(value));
        if (value <= 0x7ff) {
            return AppendByte(output, capacity, length, static_cast<char>(0xc0 | (value >> 6))) &&
                AppendByte(output, capacity, length, static_cast<char>(0x80 | (value & 0x3f)));
        }
        return value <= 0xffff &&
            AppendByte(output, capacity, length, static_cast<char>(0xe0 | (value >> 12))) &&
            AppendByte(output, capacity, length, static_cast<char>(0x80 | ((value >> 6) & 0x3f))) &&
            AppendByte(output, capacity, length, static_cast<char>(0x80 | (value & 0x3f)));
    }

    bool ReadHex4(uint32_t* value) {
        if (value == nullptr || end_ - cursor_ < 4) return false;
        uint32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = *cursor_++;
            result <<= 4;
            if (digit >= '0' && digit <= '9') result |= static_cast<uint32_t>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') result |= static_cast<uint32_t>(digit - 'a' + 10);
            else if (digit >= 'A' && digit <= 'F') result |= static_cast<uint32_t>(digit - 'A' + 10);
            else return false;
        }
        *value = result;
        return true;
    }

    bool ReadString(char* output, size_t capacity) {
        SkipWhitespace();
        if (output == nullptr || capacity == 0 || cursor_ >= end_ || *cursor_++ != '"') return false;
        size_t length = 0;
        output[0] = '\0';
        while (cursor_ < end_) {
            char value = *cursor_++;
            if (value == '"') return length != 0;
            if (static_cast<unsigned char>(value) < 0x20) return false;
            if (value != '\\') {
                if (!AppendByte(output, capacity, &length, value)) return false;
                continue;
            }
            if (cursor_ >= end_) return false;
            value = *cursor_++;
            if (value == '"' || value == '\\' || value == '/') {
                if (!AppendByte(output, capacity, &length, value)) return false;
            } else if (value == 'b' || value == 'f' || value == 'n' || value == 'r' || value == 't') {
                const char decoded = value == 'b' ? '\b' : value == 'f' ? '\f' :
                    value == 'n' ? '\n' : value == 'r' ? '\r' : '\t';
                if (!AppendByte(output, capacity, &length, decoded)) return false;
            } else if (value == 'u') {
                uint32_t code_point = 0;
                if (!ReadHex4(&code_point) || (code_point >= 0xd800 && code_point <= 0xdfff) ||
                    !AppendCodePoint(output, capacity, &length, code_point)) return false;
            } else {
                return false;
            }
        }
        return false;
    }

    bool ReadStoredString(const char** output) {
        if (output == nullptr || string_pool_ == nullptr ||
            string_pool_used_ >= string_pool_capacity_) return false;
        char* start = string_pool_ + string_pool_used_;
        if (!ReadString(start, string_pool_capacity_ - string_pool_used_)) return false;
        string_pool_used_ += TextLength(start) + 1;
        *output = start;
        return true;
    }

    bool ReadUnsigned(uint32_t* output) {
        SkipWhitespace();
        if (output == nullptr || cursor_ >= end_ || *cursor_ < '0' || *cursor_ > '9') return false;
        uint32_t value = 0;
        do {
            const uint32_t digit = static_cast<uint32_t>(*cursor_++ - '0');
            if (value > (UINT32_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        } while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9');
        *output = value;
        return true;
    }

    bool ReadStation(RadioStation* station) {
        if (station == nullptr || !Take('{')) return false;
        bool have_name = false;
        bool have_url = false;
        bool first = true;
        while (true) {
            SkipWhitespace();
            if (Take('}')) break;
            if (!first && !Take(',')) return false;
            first = false;
            char key[16]{};
            if (!ReadString(key, sizeof(key)) || !Take(':')) return false;
            if (TextEquals(key, "name")) {
                if (!ReadStoredString(&station->name) ||
                    TextLength(station->name) > 96) return false;
                have_name = true;
            } else if (TextEquals(key, "url")) {
                if (!ReadStoredString(&station->url) ||
                    TextLength(station->url) > 1024) return false;
                have_url = true;
            } else if (!SkipValue()) {
                return false;
            }
        }
        return have_name && have_url &&
            (TextStartsWith(station->url, "http://") ||
             TextStartsWith(station->url, "https://"));
    }

    bool ReadStationArray(RadioStation* stations, uint32_t capacity, uint32_t* count) {
        if (!Take('[')) return false;
        *count = 0;
        bool first = true;
        while (true) {
            SkipWhitespace();
            if (Take(']')) return true;
            if (!first && !Take(',')) return false;
            first = false;
            if (*count >= capacity || !ReadStation(&stations[*count])) return false;
            ++*count;
        }
    }

    bool SkipValue() {
        SkipWhitespace();
        if (cursor_ >= end_) return false;
        if (*cursor_ == '"') {
            ++cursor_;
            bool escaped = false;
            while (cursor_ < end_) {
                const char value = *cursor_++;
                if (escaped) escaped = false;
                else if (value == '\\') escaped = true;
                else if (value == '"') return true;
            }
            return false;
        }
        if (*cursor_ == '{' || *cursor_ == '[') {
            const char open = *cursor_++;
            const char close = open == '{' ? '}' : ']';
            bool in_string = false;
            bool escaped = false;
            int depth = 1;
            while (cursor_ < end_ && depth > 0) {
                const char value = *cursor_++;
                if (in_string) {
                    if (escaped) escaped = false;
                    else if (value == '\\') escaped = true;
                    else if (value == '"') in_string = false;
                } else if (value == '"') in_string = true;
                else if (value == open) ++depth;
                else if (value == close) --depth;
            }
            return depth == 0;
        }
        while (cursor_ < end_ && *cursor_ != ',' && *cursor_ != '}' && *cursor_ != ']') ++cursor_;
        return true;
    }

    const char* cursor_;
    const char* end_;
    char* string_pool_;
    size_t string_pool_capacity_;
    size_t string_pool_used_ = 0;
};

bool HasControlApi(const metalio_app_host_api_t* api) {
    return api != nullptr && api->struct_size >=
        offsetof(metalio_app_host_api_t, set_action_picker_selected) +
        sizeof(api->set_action_picker_selected);
}

bool LoadStations(uint32_t* selected) {
    uint32_t size = 0;
    if (s_app.api->config_read(s_app.launch->host_context, "stations.json",
            s_configuration, kConfigurationCapacity, &size) != METALIO_APP_STORAGE_OK ||
        size == 0 || size > kConfigurationCapacity) return false;
    s_configuration[size] = '\0';
    uint32_t default_station = 1;
    JsonReader reader(reinterpret_cast<const char*>(s_configuration), size,
        s_app.station_strings, sizeof(s_app.station_strings));
    if (!reader.Parse(s_app.stations, kMaximumStations, &s_app.station_count, &default_station)) return false;
    for (uint32_t index = 0; index < s_app.station_count; ++index) {
        s_app.station_labels[index] = s_app.stations[index].name;
    }
    *selected = default_station - 1;
    return true;
}

void RestoreLastStation(uint32_t* selected) {
    if (selected == nullptr) return;
    uint32_t size = 0;
    if (s_app.api->config_read(s_app.launch->host_context, kSavedStationPath,
            s_saved_station_url, kSavedStationUrlCapacity, &size) !=
            METALIO_APP_STORAGE_OK ||
        size == 0 || size > kSavedStationUrlCapacity) {
        return;
    }
    s_saved_station_url[size] = '\0';
    const char* saved_url = reinterpret_cast<const char*>(s_saved_station_url);
    for (uint32_t index = 0; index < s_app.station_count; ++index) {
        if (TextEquals(s_app.stations[index].url, saved_url)) {
            *selected = index;
            s_app.saved_station = index;
            return;
        }
    }
}

void SavePlayingStation(uint32_t index) {
    if (index >= s_app.station_count || index == s_app.saved_station) return;
    const char* url = s_app.stations[index].url;
    const size_t length = TextLength(url);
    if (length == 0 || length > kSavedStationUrlCapacity) return;
    if (s_app.api->config_write(s_app.launch->host_context, kSavedStationPath,
            reinterpret_cast<const uint8_t*>(url),
            static_cast<uint32_t>(length)) == METALIO_APP_STORAGE_OK) {
        s_app.saved_station = index;
    }
}

void SetStationText(const char* text, uint32_t color) {
    s_app.api->set_label_text(s_app.launch->host_context, s_app.station_label, text);
    s_app.api->set_label_color(s_app.launch->host_context, s_app.station_label, color);
}

void QueueTune(uint32_t index) {
    if (index >= s_app.station_count) return;
    s_app.requested_station = index;
    s_app.media_state = METALIO_APP_MEDIA_CONNECTING;
    s_app.tune_pending = true;
    SetStationText("接收中...", s_app.theme.muted);
}

void OnStationSelected(void*, uint32_t index) {
    if (index == s_app.requested_station && s_app.media_state != METALIO_APP_MEDIA_ERROR) return;
    if ((s_app.capabilities & METALIO_APP_CAP_HAPTICS) != 0 && s_app.api->play_haptic != nullptr) {
        s_app.api->play_haptic(s_app.launch->host_context, METALIO_APP_HAPTIC_CLICK);
    }
    QueueTune(index);
}

void OnVolumeChanged(void*, int32_t value) {
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    s_app.volume = static_cast<uint8_t>(value);
    char text[8]{};
    FormatPercent(text, sizeof(text), s_app.volume);
    s_app.api->set_label_text(s_app.launch->host_context, s_app.volume_label, text);
    s_app.api->media_set_volume(s_app.launch->host_context, s_app.volume);
}

void UpdateSpectrum() {
    uint8_t levels[METALIO_APP_MEDIA_SPECTRUM_BANDS]{};
    if (s_app.media_state == METALIO_APP_MEDIA_PLAYING) {
        s_app.api->media_get_spectrum(s_app.launch->host_context, levels);
    }
    for (uint32_t index = 0; index < METALIO_APP_MEDIA_SPECTRUM_BANDS; ++index) {
        const int16_t height = static_cast<int16_t>(10 +
            static_cast<uint32_t>(levels[index]) * (kSpectrumMaximumHeight - 10) / 255);
        const int16_t x = static_cast<int16_t>(60 + index * (kSpectrumBarWidth + kSpectrumGap));
        s_app.api->set_widget_bounds(s_app.launch->host_context, s_app.spectrum[index],
            x, static_cast<int16_t>(kSpectrumBottom - height), kSpectrumBarWidth, height);
    }
}

void Refresh(void*) {
    if (s_app.tune_pending) {
        s_app.tune_pending = false;
        const uint32_t station = s_app.requested_station;
        if (s_app.api->media_start(s_app.launch->host_context,
                s_app.stations[station].url) != 0) {
            s_app.media_state = METALIO_APP_MEDIA_ERROR;
            SetStationText("接收失败", s_app.theme.danger);
        }
    }
    metalio_app_media_state_t state = METALIO_APP_MEDIA_IDLE;
    if (s_app.api->media_get_state(s_app.launch->host_context, &state) == 0 && state != s_app.media_state) {
        s_app.media_state = state;
        if (state == METALIO_APP_MEDIA_PLAYING) {
            s_app.playing_station = s_app.requested_station;
            SavePlayingStation(s_app.playing_station);
            SetStationText(s_app.stations[s_app.playing_station].name, s_app.theme.text);
        } else if (state == METALIO_APP_MEDIA_CONNECTING) {
            SetStationText("接收中...", s_app.theme.muted);
        } else if (state == METALIO_APP_MEDIA_ERROR) {
            SetStationText("接收失败", s_app.theme.danger);
        }
    }
    UpdateSpectrum();
}

void ApplyTheme(const metalio_app_theme_t& theme) {
    s_app.theme = theme;
    s_app.api->set_background(s_app.launch->host_context, theme.background);
    if (s_app.media_state == METALIO_APP_MEDIA_ERROR) SetStationText("接收失败", theme.danger);
    else if (s_app.media_state == METALIO_APP_MEDIA_PLAYING) SetStationText(s_app.stations[s_app.playing_station].name, theme.text);
    else SetStationText("接收中...", theme.muted);
    for (metalio_app_widget_t bar : s_app.spectrum) {
        s_app.api->set_rect_color(s_app.launch->host_context, bar, theme.accent);
    }
    s_app.api->set_rect_border(s_app.launch->host_context, s_app.upper_divider, theme.border, 1);
    s_app.api->set_rect_border(s_app.launch->host_context, s_app.lower_divider, theme.border, 1);
    s_app.api->set_slider_colors(s_app.launch->host_context, s_app.volume_slider,
        theme.border, theme.accent, theme.accent);
    s_app.api->set_label_color(s_app.launch->host_context, s_app.volume_label, theme.muted);
    if (s_app.volume_icon != 0) s_app.api->set_icon_color(s_app.launch->host_context, s_app.volume_icon, theme.muted);
}

void OnThemeChanged(void*, const metalio_app_theme_t* theme) {
    if (theme != nullptr) ApplyTheme(*theme);
}

bool BuildInterface(uint32_t selected) {
    char volume_text[8]{};
    FormatPercent(volume_text, sizeof(volume_text), s_app.volume);
    if (s_app.api->add_label_ex(s_app.launch->host_context, "接收中...", 54, 40, 612, 70,
            s_app.theme.muted, METALIO_APP_FONT_LARGE_BOLD, &s_app.station_label) != 0 ||
        s_app.api->set_label_alignment(s_app.launch->host_context, s_app.station_label,
            METALIO_APP_TEXT_ALIGN_CENTER) != 0) return false;
    for (uint32_t index = 0; index < METALIO_APP_MEDIA_SPECTRUM_BANDS; ++index) {
        const int16_t x = static_cast<int16_t>(60 + index * (kSpectrumBarWidth + kSpectrumGap));
        if (s_app.api->add_rect(s_app.launch->host_context, x, kSpectrumBottom - 10,
                kSpectrumBarWidth, 10, s_app.theme.accent, 10, &s_app.spectrum[index]) != 0) return false;
    }
    if (s_app.api->add_rect(s_app.launch->host_context, 50, 424, 620, 1,
            s_app.theme.border, 0, &s_app.upper_divider) != 0 ||
        s_app.api->add_rect(s_app.launch->host_context, 50, 503, 620, 1,
            s_app.theme.border, 0, &s_app.lower_divider) != 0 ||
        s_app.api->add_slider(s_app.launch->host_context, 95, 450, 500, 24, 0, 100, s_app.volume,
            s_app.theme.border, s_app.theme.accent, s_app.theme.accent,
            OnVolumeChanged, nullptr, &s_app.volume_slider) != 0 ||
        s_app.api->add_label_ex(s_app.launch->host_context, volume_text, 612, 444, 58, 36,
            s_app.theme.muted, METALIO_APP_FONT_SMALL_BOLD, &s_app.volume_label) != 0 ||
        s_app.api->set_label_alignment(s_app.launch->host_context, s_app.volume_label,
            METALIO_APP_TEXT_ALIGN_RIGHT) != 0 ||
        s_app.api->add_action_picker(s_app.launch->host_context, s_app.station_labels,
            s_app.station_count, selected, OnStationSelected, nullptr, &s_app.station_picker) != 0) return false;
    if ((s_app.capabilities & METALIO_APP_CAP_UI_ICONS) != 0 &&
        s_app.api->add_icon(s_app.launch->host_context, METALIO_APP_ICON_RADIO,
            50, 445, 30, 30, s_app.theme.muted, &s_app.volume_icon) != 0) return false;
    return true;
}

void BuildConfigurationError() {
    metalio_app_widget_t title = 0;
    metalio_app_widget_t detail = 0;
    s_app.api->set_background(s_app.launch->host_context, s_app.theme.background);
    if (s_app.api->add_label_ex(s_app.launch->host_context, "电台配置无效", 60, 188, 600, 58,
            s_app.theme.danger, METALIO_APP_FONT_LARGE_BOLD, &title) == 0) {
        s_app.api->set_label_alignment(s_app.launch->host_context, title, METALIO_APP_TEXT_ALIGN_CENTER);
    }
    if (s_app.api->add_label_ex(s_app.launch->host_context,
            "请检查 App 私有目录中的 stations.json", 60, 254, 600, 40,
            s_app.theme.muted, METALIO_APP_FONT_SMALL_BOLD, &detail) == 0) {
        s_app.api->set_label_alignment(s_app.launch->host_context, detail, METALIO_APP_TEXT_ALIGN_CENTER);
    }
}

}  // namespace

extern "C" int main(int argc, char* argv[]) {
    if (argc != 2 || argv == nullptr || argv[0] == nullptr || argv[1] == nullptr) return 1;
    s_app.api = reinterpret_cast<const metalio_app_host_api_t*>(argv[0]);
    s_app.launch = reinterpret_cast<const metalio_app_launch_context_t*>(argv[1]);
    if (s_app.api->abi_version != METALIO_APP_ABI_VERSION || !HasControlApi(s_app.api) ||
        s_app.launch->abi_version != METALIO_APP_ABI_VERSION ||
        s_app.launch->struct_size < sizeof(metalio_app_launch_context_t)) return 2;
    s_app.capabilities = s_app.api->get_capabilities(s_app.launch->host_context);
    const metalio_app_capabilities_t required = METALIO_APP_CAP_MEDIA_HLS |
        METALIO_APP_CAP_MEDIA_SPECTRUM | METALIO_APP_CAP_UI_DRAW |
        METALIO_APP_CAP_UI_THEME | METALIO_APP_CAP_APP_STORAGE |
        METALIO_APP_CAP_UI_CONTROLS;
    if ((s_app.capabilities & required) != required ||
        s_app.api->get_theme(s_app.launch->host_context, &s_app.theme) != 0) return 3;
    uint32_t selected = 0;
    if (!LoadStations(&selected)) {
        BuildConfigurationError();
        return 0;
    }
    RestoreLastStation(&selected);
    if (s_app.api->media_get_volume(s_app.launch->host_context, &s_app.volume) != 0) s_app.volume = 50;
    if (!BuildInterface(selected) ||
        s_app.api->set_theme_callback(s_app.launch->host_context, OnThemeChanged, nullptr) != 0 ||
        s_app.api->set_interval(s_app.launch->host_context, 40, Refresh, nullptr) != 0) return 5;
    ApplyTheme(s_app.theme);
    QueueTune(selected);
    return 0;
}
