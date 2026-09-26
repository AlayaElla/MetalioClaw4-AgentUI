#include "ui_dispatcher.h"
#include "lvgl.h"
#include <vector>
namespace { std::vector<std::function<void()>> posts; std::vector<lv_timer_t*> timers; bool post_ok=true,timer_ok=true; }
bool UiDispatcher::Post(std::function<void()> f){if(!post_ok)return false;posts.push_back(std::move(f));return true;}
lv_timer_t* lv_timer_create(void(*cb)(lv_timer_t*),uint32_t,void* data){if(!timer_ok)return nullptr;auto*t=new lv_timer_t{cb,data,false};timers.push_back(t);return t;} void lv_timer_ready(lv_timer_t*){} void lv_timer_delete(lv_timer_t*t){t->dead=true;} void* lv_timer_get_user_data(lv_timer_t*t){return t->data;}
namespace ui_test { void RunPosts(){auto q=std::move(posts);posts.clear();for(auto&f:q)f();} void Tick(){auto q=timers;for(auto*t:q)if(!t->dead)t->cb(t);} void SetPost(bool v){post_ok=v;} void SetTimer(bool v){timer_ok=v;} }
