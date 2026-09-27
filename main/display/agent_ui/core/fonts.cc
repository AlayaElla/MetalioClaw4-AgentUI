#include "fonts.h"

#include <string>

#include "esp_log.h"
#include "sdkconfig.h"

#if !CONFIG_AGENT_UI_FONTS_SD_ONLY
LV_FONT_DECLARE(font_agent_small_18);
LV_FONT_DECLARE(font_agent_medium_28);
LV_FONT_DECLARE(font_agent_large_56);
LV_FONT_DECLARE(font_agent_small_bold_18);
LV_FONT_DECLARE(font_agent_medium_bold_28);
LV_FONT_DECLARE(font_agent_large_bold_56);
#endif
LV_FONT_DECLARE(font_agent_emoji_18);
LV_FONT_DECLARE(font_agent_emoji_28);
LV_FONT_DECLARE(font_agent_emoji_56);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(font_agent_home_name_bold_35);
LV_FONT_DECLARE(font_agent_home_number_bold_70);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

namespace agent_ui::fonts {
namespace {

constexpr const char* kTag = "AgentFonts";

// Resolves a CJK face: an SD-card .bin (scripts/generate_agent_fonts.py
// --bin) wins when present; otherwise the compiled-in array, or a
// placeholder if this build excluded the arrays and the card is missing.
const lv_font_t* Resolve(const char* name, const lv_font_t* builtin) {
#if CONFIG_AGENT_UI_FONTS_ON_SD
    const std::string path =
        std::string("S:/sdcard/metalio/fonts/") + name + ".bin";
    lv_font_t* loaded = lv_binfont_create(path.c_str());
    if (loaded != nullptr) {
        ESP_LOGI(kTag, "Loaded SD font: %s", name);
        return loaded;
    }
    ESP_LOGW(kTag, "SD font missing: %s", name);
#else
    (void)name;
#endif
    return builtin != nullptr ? builtin : &lv_font_montserrat_18;
}

#if !CONFIG_AGENT_UI_FONTS_SD_ONLY
#define AGENT_BUILTIN_FONT(name) (&name)
#else
#define AGENT_BUILTIN_FONT(name) (nullptr)
#endif

// Each face gets its own slot instantiated from this template, so the copied
// font (built-in arrays are const, bin fonts need a fallback override) has
// stable, per-callsite storage.
template <int Slot>
const lv_font_t* CopyWithFallback(const char* name, const lv_font_t* builtin,
                                  const lv_font_t* fallback) {
    static lv_font_t font;
    font = *Resolve(name, builtin);
    font.fallback = fallback;
    return &font;
}

}  // namespace

const lv_font_t* Large() {
    return CopyWithFallback<0>("font_agent_large_56",
                               AGENT_BUILTIN_FONT(font_agent_large_56),
                               &font_agent_emoji_56);
}
const lv_font_t* Medium() {
    return CopyWithFallback<1>("font_agent_medium_28",
                               AGENT_BUILTIN_FONT(font_agent_medium_28),
                               &font_agent_emoji_28);
}
const lv_font_t* Small() {
    return CopyWithFallback<2>("font_agent_small_18",
                               AGENT_BUILTIN_FONT(font_agent_small_18),
                               &font_agent_emoji_18);
}
const lv_font_t* LargeBold() {
    return CopyWithFallback<3>("font_agent_large_bold_56",
                               AGENT_BUILTIN_FONT(font_agent_large_bold_56),
                               &font_agent_emoji_56);
}
const lv_font_t* MediumBold() {
    return CopyWithFallback<4>("font_agent_medium_bold_28",
                               AGENT_BUILTIN_FONT(font_agent_medium_bold_28),
                               Medium());
}
const lv_font_t* SmallBold() {
    return CopyWithFallback<5>("font_agent_small_bold_18",
                               AGENT_BUILTIN_FONT(font_agent_small_bold_18),
                               Small());
}
const lv_font_t* Keyboard() { return &lv_font_montserrat_18; }
const lv_font_t* HomeNameBold() { return &font_agent_home_name_bold_35; }
const lv_font_t* HomeNumberBold() { return &font_agent_home_number_bold_70; }
const lv_font_t* Icon() { return &font_awesome_20_4; }
const lv_font_t* IconLarge() { return &font_awesome_30_4; }

}  // namespace agent_ui::fonts
