list(APPEND AGENT_UI_SOURCES
    "display/agent_ui/apps/camera/camera_adapter.cc"
    "display/agent_ui/apps/camera/camera_capture_backend.cc"
    "display/agent_ui/apps/camera/camera_controller.cc"
    "display/agent_ui/apps/camera/camera_gallery_repository.cc"
    "display/agent_ui/apps/camera/camera_image_codec.cc"
    "display/agent_ui/apps/camera/camera_image_decoder.cc"
    "display/agent_ui/apps/camera/camera_module.cc"
    "display/agent_ui/apps/camera/camera_view.cc"
    "display/agent_ui/apps/camera/effects/ascii_effect.cc"
    "display/agent_ui/apps/camera/effects/black_white_effect.cc"
    "display/agent_ui/apps/camera/effects/camera_effects.cc"
    "display/agent_ui/apps/camera/effects/mosaic_effect.cc"
    "display/agent_ui/apps/camera/effects/original_effect.cc"
    "display/agent_ui/apps/camera/effects/print_effect.cc"
)

list(APPEND AGENT_UI_INCLUDE_DIRS
    "display/agent_ui/apps/camera"
    "display/agent_ui/apps/camera/effects"
)
