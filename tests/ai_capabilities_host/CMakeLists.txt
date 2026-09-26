cmake_minimum_required(VERSION 3.20)
project(ai_capabilities_host_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(ai-capabilities-host
    ai_capabilities_host_test.cc
    "${ROOT}/main/ai/ai_capabilities.cc"
    "${ROOT}/main/ai/ai_availability.cc"
    "${ROOT}/managed_components/espressif__cjson/cJSON/cJSON.c")
target_include_directories(ai-capabilities-host PRIVATE
    "${ROOT}/main"
    "${ROOT}/managed_components/espressif__cjson/cJSON")
