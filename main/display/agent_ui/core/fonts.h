#pragma once

#include "lvgl.h"

namespace agent_ui::fonts {

const lv_font_t* Large();
const lv_font_t* Medium();
const lv_font_t* Small();
const lv_font_t* LargeBold();
const lv_font_t* MediumBold();
const lv_font_t* SmallBold();
// LVGL's keyboard requires its built-in symbol glyphs in addition to ASCII.
const lv_font_t* Keyboard();
const lv_font_t* HomeNameBold();
const lv_font_t* HomeNumberBold();

const lv_font_t* Icon();
const lv_font_t* IconLarge();

}  // namespace agent_ui::fonts
