# Runtime and shared layers are listed first; each app owns the manifest next
# to its vertical slice under apps/<app>/.
set(AGENT_UI_SOURCES
    "display/agent_ui/agent_ui_runtime.cc"
    "display/agent_ui/core/app_shell.cc"
    "display/agent_ui/core/fonts.cc"
    "display/agent_ui/core/idle_power.cc"
    "display/agent_ui/core/navigation.cc"
    "display/agent_ui/core/power_key.cc"
    "display/agent_ui/core/status_bar.cc"
    "display/agent_ui/core/status_signal_assets.cc"
    "display/agent_ui/core/theme.cc"
    "display/agent_ui/core/ui_utils.cc"
    "display/agent_ui/components/expression_acceleration.cc"
    "display/agent_ui/components/expression_player.cc"
    "display/agent_ui/components/haptic_feedback.cc"
    "display/agent_ui/components/system_keyboard.cc"
)

set(AGENT_UI_INCLUDE_DIRS
    "display/agent_ui"
    "display/agent_ui/core"
    "display/agent_ui/components"
)

include("${CMAKE_CURRENT_LIST_DIR}/apps/bluetooth/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/boot/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/camera/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/codex/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/display_debug/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/files/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/home/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/network/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/phone/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/power/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/settings/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/apps/standby/sources.cmake")
