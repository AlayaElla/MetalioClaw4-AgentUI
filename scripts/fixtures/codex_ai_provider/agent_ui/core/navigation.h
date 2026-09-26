#pragma once
namespace agent_ui {
enum class ScreenId { Home, Codex };
class Navigation {
public:
 static Navigation& Get();
 ScreenId current() const;
 void Open(ScreenId);
private:
 ScreenId current_ = ScreenId::Home;
};
}
