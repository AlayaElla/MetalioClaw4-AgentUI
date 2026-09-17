# Validate the resolved Kconfig values, including builds using a custom SDKCONFIG.
# Wrong transport defaults can take over this board's I2C and audio GPIOs.
if(NOT CONFIG_BOARD_TYPE_METALIO_CLAW_4 OR NOT CONFIG_ESP_HOSTED_ENABLED)
    return()
endif()

if(NOT CONFIG_ESP_WIFI_REMOTE_ENABLED OR
   NOT CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED OR
   NOT CONFIG_SLAVE_IDF_TARGET_ESP32C5 OR
   NOT CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5 OR
   NOT CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE)
    message(FATAL_ERROR
        "Metalio Claw 4 Wi-Fi requires ESP32-C5 in both Wi-Fi Remote and "
        "ESP-Hosted, with SDIO transport. Check sdkconfig.defaults.esp32p4.")
endif()

function(metalio_require_wifi_value name expected)
    if(NOT DEFINED ${name} OR NOT "${${name}}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "Metalio Claw 4 Wi-Fi requires ${name}=${expected}; "
            "resolved value is '${${name}}'. Check sdkconfig.defaults.esp32p4.")
    endif()
endfunction()

metalio_require_wifi_value(CONFIG_ESP_HOSTED_SDIO_SLOT 1)
metalio_require_wifi_value(CONFIG_ESP_HOSTED_SDIO_PIN_CMD 50)
metalio_require_wifi_value(CONFIG_ESP_HOSTED_SDIO_PIN_CLK 51)
metalio_require_wifi_value(CONFIG_ESP_HOSTED_SDIO_PIN_D0 49)
metalio_require_wifi_value(CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE 54)
if(NOT CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH)
    message(FATAL_ERROR "Metalio Claw 4 requires active-high C5 reset on GPIO54.")
endif()
if(CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS)
    metalio_require_wifi_value(CONFIG_ESP_HOSTED_SDIO_PIN_D1 34)
    metalio_require_wifi_value(CONFIG_ESP_HOSTED_SDIO_PIN_D2 31)
    metalio_require_wifi_value(CONFIG_ESP_HOSTED_SDIO_PIN_D3 53)
endif()
