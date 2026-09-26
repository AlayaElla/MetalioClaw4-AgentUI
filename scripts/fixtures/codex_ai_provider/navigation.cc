#include "agent_ui/core/navigation.h"
namespace agent_ui {
Navigation& Navigation::Get() { static Navigation value; return value; }
ScreenId Navigation::current() const { return current_; }
void Navigation::Open(ScreenId value) { current_ = value; }
}
