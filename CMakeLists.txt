# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.1")

# Make the local ESP-IDF components available to this firmware.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(agent)

# The Dashboard login response uses more than one Set-Cookie header. Keep the
# small esp-ml307 transport extension reproducible even though managed_components
# is intentionally not committed. The component manager may restore the pristine
# dependency during reconfigure, so apply the patch idempotently after project().
find_package(Git QUIET)
set(ESP_ML307_DIR "${CMAKE_SOURCE_DIR}/managed_components/78__esp-ml307")
set(ESP_ML307_HEADERS_PATCH
    "${CMAKE_SOURCE_DIR}/patches/esp-ml307-multi-response-headers.patch")
set(ESP_ML307_TCP_LIFETIME_PATCH
    "${CMAKE_SOURCE_DIR}/patches/esp-ml307-tcp-receive-task-lifetime.patch")
if(GIT_FOUND AND EXISTS "${ESP_ML307_DIR}/include/http.h" AND
   EXISTS "${ESP_ML307_HEADERS_PATCH}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check "${ESP_ML307_HEADERS_PATCH}"
        WORKING_DIRECTORY "${ESP_ML307_DIR}"
        RESULT_VARIABLE ESP_ML307_PATCH_NEEDED
        OUTPUT_QUIET ERROR_QUIET)
    if(ESP_ML307_PATCH_NEEDED EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${ESP_ML307_HEADERS_PATCH}"
            WORKING_DIRECTORY "${ESP_ML307_DIR}"
            RESULT_VARIABLE ESP_ML307_PATCH_RESULT)
        if(NOT ESP_ML307_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to patch esp-ml307 response header support")
        endif()
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --reverse --check
                    "${ESP_ML307_HEADERS_PATCH}"
            WORKING_DIRECTORY "${ESP_ML307_DIR}"
            RESULT_VARIABLE ESP_ML307_PATCH_ALREADY_APPLIED
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT ESP_ML307_PATCH_ALREADY_APPLIED EQUAL 0)
            message(FATAL_ERROR
                "esp-ml307 sources do not match the tested multi-header patch")
        endif()
    endif()
endif()

if(GIT_FOUND AND EXISTS "${ESP_ML307_DIR}/src/esp/esp_tcp.cc" AND
   EXISTS "${ESP_ML307_TCP_LIFETIME_PATCH}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check "${ESP_ML307_TCP_LIFETIME_PATCH}"
        WORKING_DIRECTORY "${ESP_ML307_DIR}"
        RESULT_VARIABLE ESP_ML307_TCP_PATCH_NEEDED
        OUTPUT_QUIET ERROR_QUIET)
    if(ESP_ML307_TCP_PATCH_NEEDED EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${ESP_ML307_TCP_LIFETIME_PATCH}"
            WORKING_DIRECTORY "${ESP_ML307_DIR}"
            RESULT_VARIABLE ESP_ML307_TCP_PATCH_RESULT)
        if(NOT ESP_ML307_TCP_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to patch esp-ml307 TCP task lifetime")
        endif()
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --reverse --check
                    "${ESP_ML307_TCP_LIFETIME_PATCH}"
            WORKING_DIRECTORY "${ESP_ML307_DIR}"
            RESULT_VARIABLE ESP_ML307_TCP_PATCH_ALREADY_APPLIED
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT ESP_ML307_TCP_PATCH_ALREADY_APPLIED EQUAL 0)
            message(FATAL_ERROR
                "esp-ml307 sources do not match the tested TCP lifetime patch")
        endif()
    endif()
endif()

set(ESP_VIDEO_DIR "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_video")
set(ESP_VIDEO_SENSOR_FORMAT_PATCH
    "${CMAKE_SOURCE_DIR}/patches/esp-video-select-sensor-format.patch")
if(GIT_FOUND AND
   EXISTS "${ESP_VIDEO_DIR}/src/device/esp_video_device_common.c" AND
   EXISTS "${ESP_VIDEO_SENSOR_FORMAT_PATCH}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check
                "${ESP_VIDEO_SENSOR_FORMAT_PATCH}"
        WORKING_DIRECTORY "${ESP_VIDEO_DIR}"
        RESULT_VARIABLE ESP_VIDEO_PATCH_NEEDED
        OUTPUT_QUIET ERROR_QUIET)
    if(ESP_VIDEO_PATCH_NEEDED EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply
                    "${ESP_VIDEO_SENSOR_FORMAT_PATCH}"
            WORKING_DIRECTORY "${ESP_VIDEO_DIR}"
            RESULT_VARIABLE ESP_VIDEO_PATCH_RESULT)
        if(NOT ESP_VIDEO_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR
                "Failed to patch esp-video sensor format selection")
        endif()
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --reverse --check
                    "${ESP_VIDEO_SENSOR_FORMAT_PATCH}"
            WORKING_DIRECTORY "${ESP_VIDEO_DIR}"
            RESULT_VARIABLE ESP_VIDEO_PATCH_ALREADY_APPLIED
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT ESP_VIDEO_PATCH_ALREADY_APPLIED EQUAL 0)
            message(FATAL_ERROR
                "esp-video sources do not match the tested sensor format patch")
        endif()
    endif()
endif()

set(ESP_CAM_SENSOR_DIR
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_cam_sensor")
set(ESP_CAM_SENSOR_OV2710_720_PATCH
    "${CMAKE_SOURCE_DIR}/patches/esp-cam-sensor-ov2710-720x720.patch")
if(GIT_FOUND AND
   EXISTS "${ESP_CAM_SENSOR_DIR}/sensors/ov2710/ov2710.c" AND
   EXISTS "${ESP_CAM_SENSOR_OV2710_720_PATCH}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check
                "${ESP_CAM_SENSOR_OV2710_720_PATCH}"
        WORKING_DIRECTORY "${ESP_CAM_SENSOR_DIR}"
        RESULT_VARIABLE ESP_CAM_SENSOR_PATCH_NEEDED
        OUTPUT_QUIET ERROR_QUIET)
    if(ESP_CAM_SENSOR_PATCH_NEEDED EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply
                    "${ESP_CAM_SENSOR_OV2710_720_PATCH}"
            WORKING_DIRECTORY "${ESP_CAM_SENSOR_DIR}"
            RESULT_VARIABLE ESP_CAM_SENSOR_PATCH_RESULT)
        if(NOT ESP_CAM_SENSOR_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR
                "Failed to patch OV2710 720x720 sensor mode")
        endif()
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --reverse --check
                    "${ESP_CAM_SENSOR_OV2710_720_PATCH}"
            WORKING_DIRECTORY "${ESP_CAM_SENSOR_DIR}"
            RESULT_VARIABLE ESP_CAM_SENSOR_PATCH_ALREADY_APPLIED
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT ESP_CAM_SENSOR_PATCH_ALREADY_APPLIED EQUAL 0)
            message(FATAL_ERROR
                "esp_cam_sensor sources do not match the tested OV2710 patch")
        endif()
    endif()
endif()

# Override esp-sr packed model with custom wakeword when present
if(EXISTS "${CMAKE_SOURCE_DIR}/wakeword/srmodels.bin")
    if(TARGET srmodels_bin)
        add_custom_target(custom_wakeword_srmodels ALL
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_SOURCE_DIR}/wakeword/srmodels.bin"
                "${CMAKE_BINARY_DIR}/srmodels/srmodels.bin"
            COMMENT "Override srmodels.bin with custom wakeword"
        )
        add_dependencies(custom_wakeword_srmodels srmodels_bin)
        if(TARGET flash)
            add_dependencies(flash custom_wakeword_srmodels)
        endif()
        message(STATUS "Custom wakeword enabled: wakeword/srmodels.bin -> build/srmodels/srmodels.bin")
    else()
        message(WARNING "wakeword/srmodels.bin exists but srmodels_bin target not found; custom wakeword override skipped")
    endif()
endif()
