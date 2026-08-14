list(APPEND AGENT_UI_SOURCES
    "display/agent_ui/apps/external_apps/external_app_manager.cc"
    "display/agent_ui/apps/external_apps/external_app_runtime.cc"
    "display/agent_ui/apps/external_apps/external_apps_view.cc"
)

list(APPEND AGENT_UI_INCLUDE_DIRS
    "display/agent_ui/apps/external_apps"
    "../external_apps/sdk"
)
