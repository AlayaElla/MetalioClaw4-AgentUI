#pragma once

namespace agent_ui::camera {

// Registers camera.control after the Agent UI runtime has created its module.
// Repeated registration is harmless and supports runtime reloads.
void RegisterAiProvider();
void UnregisterAiProvider();

}  // namespace agent_ui::camera
