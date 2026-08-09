#pragma once

#include <cstddef>

#include "lvgl.h"

namespace agent_ui {

void InitializeExpressionAcceleration();
void RegisterExpressionA8Buffer(const void* buffer, size_t size);
void UnregisterExpressionA8Buffer(const void* buffer);

}  // namespace agent_ui
