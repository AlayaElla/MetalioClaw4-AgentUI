idf_component_register(
    SRCS "codex_ws_client.cc" "codex_ws_pipeline.cc"
    INCLUDE_DIRS "."
    REQUIRES esp_websocket_client json nvs_flash mbedtls
)
