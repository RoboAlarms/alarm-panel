# Host tests: every portable component (components/core) built for the PC, and every test
# folder next to a component (components/<tier>/<name>/test) run with CTest.
#   cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host
#   (Windows: pwsh tools/host_test.ps1)
cmake_minimum_required(VERSION 3.20)
project(alarm_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/alarm_host.cmake)

enable_testing()

option(ALARM_SANITIZE "Build with address/undefined sanitizers (not on MinGW)" OFF)

# The libraries under test, with their own warning set and, on request, the sanitizers
# (applied per target, R19).
if(ALARM_SANITIZE)
    alarm_add_core_components(WARNINGS SANITIZE)
else()
    alarm_add_core_components(WARNINGS)
endif()

# Everything created from here on (Unity, the harnesses, the test executables) is in this
# directory or below it, so directory-wide options reach it, including the link options every
# test executable needs for the sanitizer runtimes.
if(ALARM_SANITIZE AND ALARM_GNU_LIKE)
    add_compile_options(${ALARM_SANITIZE_FLAGS})
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(unity STATIC third_party/unity/unity.c)
target_include_directories(unity PUBLIC third_party/unity)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE UNITY_SUPPORT_64)

# The Opus decoder and the voice's codec adapter (cmake/alarm_opus.cmake, fetched at configure
# time), for components/services/panel_voice/test.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/alarm_opus.cmake)

alarm_add_component_tests()

# The PC simulator's real Zigbee (host/ui_preview/host_zigbee_real.c; docs/features/20 testing.md
# §10, BL-143 ZB-M6): the pages' zb_* calls over zbz_panel, its zone source on a real zone_hub and
# the fake radio's virtual devices, as host_panel.c wires them - without LVGL or the engine.
alarm_add_test(host_zigbee_real
               SOURCES ui_preview/host_zigbee_real.c ui_preview/test_host_zigbee_real.c
               LIBS zigbee_zones_fake unity INCLUDES ui_preview STRICT)
