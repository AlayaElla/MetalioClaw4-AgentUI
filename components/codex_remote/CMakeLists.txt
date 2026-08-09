idf_component_register(
    SRCS "codex_ws_client.cc"
    INCLUDE_DIRS "."
    REQUIRES esp_websocket_client json nvs_flash
)
