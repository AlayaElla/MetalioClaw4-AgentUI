#pragma once
#include <functional>
class UiDispatcher { public: static bool Post(std::function<void()>); };
namespace ui_test { void RunPosts(); void Tick(); void SetPost(bool); void SetTimer(bool); }
