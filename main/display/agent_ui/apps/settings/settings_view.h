#pragma once

#include "lvgl.h"

namespace agent_ui::network { class Module; }
namespace agent_ui::bluetooth { class Module; }

namespace agent_ui {

enum class SettingsPanel : unsigned char { General, Ai, Network, Bluetooth, Language, About };

class SettingsView {
public:
    static lv_obj_t* Create();
    // UI-thread entry point used by system capabilities. It opens Settings
    // first when needed and then mounts the requested real module.
    static bool OpenPanel(SettingsPanel panel);
    static network::Module& NetworkModule();
    static bluetooth::Module& BluetoothModule();
};

}  // namespace agent_ui
