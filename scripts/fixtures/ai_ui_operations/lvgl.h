#pragma once
#include <cstdint>
struct lv_timer_t { void (*cb)(lv_timer_t*); void* data; bool dead; };
lv_timer_t* lv_timer_create(void (*)(lv_timer_t*), uint32_t, void*);
void lv_timer_ready(lv_timer_t*); void lv_timer_delete(lv_timer_t*); void* lv_timer_get_user_data(lv_timer_t*);
