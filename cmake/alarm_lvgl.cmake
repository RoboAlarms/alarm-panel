# LVGL 9.5 for the PC builds (UI preview and simulator).
#
# The copy ESP-IDF's component manager downloaded for the firmware (pinned in
# firmware/crowpanel_p4/dependencies.lock), or a shallow clone of the same tag.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/alarm_host.cmake)

set(ALARM_LVGL_TAG v9.5.0)
set(LVGL_DIR "${ALARM_REPO}/firmware/crowpanel_p4/managed_components/lvgl__lvgl" CACHE PATH
    "LVGL 9.5 sources")

# alarm_add_lvgl(CONF_DIR <dir> [WIN32_BACKEND])
#   Target "lvgl", configured by <dir>/lv_conf.h. WIN32_BACKEND adds LVGL's own Windows
#   display/input backend (the interactive simulator).
function(alarm_add_lvgl)
    cmake_parse_arguments(ARG "WIN32_BACKEND" "CONF_DIR" "" ${ARGN})
    set(dir ${LVGL_DIR})
    if(NOT EXISTS "${dir}/lvgl.h")
        include(FetchContent)
        FetchContent_Declare(lvgl GIT_REPOSITORY https://github.com/lvgl/lvgl.git
                             GIT_TAG ${ALARM_LVGL_TAG} GIT_SHALLOW TRUE)
        # Populate the source only: LVGL's own CMakeLists.txt declares a "lvgl" target too and
        # would collide with the one below (R20). One build recipe for both branches.
        FetchContent_Populate(lvgl)
        set(dir ${lvgl_SOURCE_DIR})
    endif()

    file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${dir}/src/*.c)
    add_library(lvgl STATIC ${sources})
    # The configuration directory first: its lv_conf.h wins.
    target_include_directories(lvgl PUBLIC ${ARG_CONF_DIR} ${dir})
    target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE LV_LVGL_H_INCLUDE_SIMPLE)
    target_compile_options(lvgl PRIVATE -w)
    if(ARG_WIN32_BACKEND)
        # The window backend's header pulls in <windows.h> everywhere lvgl.h goes. Without
        # WIN32_LEAN_AND_MEAN that drags in winsock, whose bind() and connect() collide with
        # the UI's own static functions of those names (ui_sources.c, ui_net.c).
        target_compile_definitions(lvgl PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(lvgl PUBLIC gdi32 user32 winmm)
    endif()
endfunction()
