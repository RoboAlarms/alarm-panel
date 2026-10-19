# Interactive panel simulator: the real alarm engine + the LVGL Pro UI in a window, with
# buttons beside the screen for the states worth looking at. Build and run:
#   pwsh tools/ui_sim.ps1 [-Board <board>]
# Windows only for now: it uses LVGL's own Win32 backend, so there is nothing to install.
#
# ALARM_BOARD picks the board profile (firmware/<board>/sdkconfig.defaults, docs/features/33 step 9):
# the simulator shows that board's layout and screen size with the pages its firmware builds,
# inside the LVGL memory the board has.
cmake_minimum_required(VERSION 3.20)
project(ui_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR
            "The interactive simulator needs LVGL's Windows backend. On Linux and macOS use "
            "the scripted preview in host/ui_preview instead.")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/alarm_host.cmake)
include(${ALARM_REPO}/cmake/alarm_lvgl.cmake)
include(${ALARM_REPO}/cmake/alarm_ui.cmake)
include(${ALARM_REPO}/cmake/alarm_board.cmake)
set(PREVIEW ${ALARM_REPO}/host/ui_preview)

set(ALARM_BOARD crowpanel_p4 CACHE STRING "Board profile: firmware/<board>/sdkconfig.defaults")
alarm_board_profile(${ALARM_BOARD})

# docs/features/36 Z6: the Z-Wave world in host_panel.c, off by default (pwsh tools/ui_sim.ps1
# -ZWave turns it on). host/ui_preview has the same switch, in a build folder of its own.
option(ALARM_SIM_ZWAVE "Build the Z-Wave world into the simulator" OFF)
message(STATUS "Simulator: board ${ALARM_BOARD}, ${ALARM_BOARD_LAYOUT}, pages: "
               "${ALARM_BOARD_UI_FEATURES}, LVGL budget ${ALARM_BOARD_LV_BUDGET_KB} KB (0 = not checked)")

# The Z-Wave world brings the Z-Wave pages (docs/features/36 Z7), as the option will in a firmware.
set(ui_features ${ALARM_BOARD_UI_FEATURES})
if(ALARM_SIM_ZWAVE)
    list(APPEND ui_features ZWAVE)
endif()

# -Locale (tools/ui_sim.ps1): the screen in another language (docs/features/37): a test language
# (en-XA, ar-XB: I18N-024) from build/i18n/pseudo, a real one (es-US) from the committed catalogs.
# Like a development build, the simulator also offers the draft languages in Settings.
set(ALARM_PREVIEW_LOCALE "" CACHE STRING "Language to start in, empty for English")
if(ALARM_PREVIEW_LOCALE MATCHES "-X[A-Z]$")
    set(I18N_CATALOG_DIR ${ALARM_REPO}/build/i18n/pseudo)
    if(NOT EXISTS ${I18N_CATALOG_DIR}/i18n_catalogs_gen.c)
        message(FATAL_ERROR "No test languages: run python tools/i18n.py pseudo --build")
    endif()
endif()

# This directory's lv_conf.h is the preview's plus the window backend.
alarm_add_lvgl(CONF_DIR ${CMAKE_CURRENT_LIST_DIR} WIN32_BACKEND)
target_compile_definitions(lvgl PUBLIC ALARM_LV_BUDGET_KB=${ALARM_BOARD_LV_BUDGET_KB}
                                       ALARM_LV_DPI=${ALARM_BOARD_LV_DPI}
                                       ${ALARM_BOARD_LV_DEFINITIONS})
alarm_add_core_components()
# The same layout and logic the board builds, so the simulator shows the real thing.
alarm_add_ui_library(LAYOUT ${ALARM_BOARD_LAYOUT} FEATURES ${ui_features}
                     DEFINITIONS ${ALARM_BOARD_DEFINITIONS})

# sim_audio.c plays the panel's sounder out of the PC's speakers (winmm/waveOut),
# rendering the same components/core/sounder mixer the board does.
# The screenshots' PNG encoder, on the C library's heap (host/ui_preview/shot_png.c); LVGL's
# own code, so not -Werror.
add_library(shot_png STATIC ${PREVIEW}/shot_png.c)
target_link_libraries(shot_png PUBLIC lvgl)
# Zigbee: the panel's real zbz_panel over the fake radio and its virtual devices
# (host_zigbee_real.c, BL-143 ZB-M6) by default; the demo model with the environment variable
# ALARM_SIM_ZIGBEE_DEMO=1 (sim_main.c).
set(ZB_HOST ${PREVIEW}/host_zigbee_demo.c ${PREVIEW}/host_zigbee_real.c ${PREVIEW}/host_link_demo.c)
add_executable(ui_sim sim_main.c sim_audio.c ${PREVIEW}/host_panel.c ${ZB_HOST})
target_include_directories(ui_sim PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${PREVIEW})
target_link_libraries(ui_sim PRIVATE alarm_ui alarm_proto automation sounder shot_png winmm zigbee_zones_fake)
# The real voice, packed at build time, as the preview has it (host/ui_preview/voice_pack.cmake).
include(${PREVIEW}/voice_pack.cmake)
alarm_host_voice_pack(ui_sim)
if(ALARM_PREVIEW_LOCALE)
    target_compile_definitions(ui_sim PRIVATE ALARM_PREVIEW_LOCALE="${ALARM_PREVIEW_LOCALE}")
endif()
# The letters pack's parts the layout commits, read by host_panel.c (docs/features/39 asset-packs.md);
# ALARM_SIM_LETTERS=0 at run time starts the simulator without them (sim_main.c).
set(letters_dir ALARM_LETTERS_DIR="${ALARM_COMPONENTS_DIR}/ui/alarm_ui/layouts/${ALARM_BOARD_LAYOUT}/alarm_ui/fonts/letters")
target_compile_definitions(ui_sim PRIVATE ${letters_dir})
target_compile_definitions(ui_sim PRIVATE ALARM_SCREEN_W=${ALARM_BOARD_WIDTH}
                                          ALARM_SCREEN_H=${ALARM_BOARD_HEIGHT})
if(ALARM_SIM_ZWAVE)
    # zwave_host and zwave_proto are already added by alarm_add_core_components() above; the
    # fake ZAC93 and its simulated devices are test sources with no test framework of their own
    # (fake_zac93.h, fake_zdev.h), so the simulator can compile them directly, as it compiles the
    # other pretend-hardware code in host_panel.c.
    set(ZWAVE_TEST ${ALARM_COMPONENTS_DIR}/core/zwave_host/test)
    target_sources(ui_sim PRIVATE ${ZWAVE_TEST}/fake_zac93.c ${ZWAVE_TEST}/fake_zdev.c
                                  ${ZWAVE_TEST}/fake_zdev_cc.c ${ZWAVE_TEST}/zw_host_crypto.c)
    target_include_directories(ui_sim PRIVATE ${ZWAVE_TEST})
    target_link_libraries(ui_sim PRIVATE zwave_host zwave_proto zwave_zones)
    target_compile_definitions(ui_sim PRIVATE ALARM_SIM_ZWAVE=1)

    # A scripted, windowless proof of the Z-Wave world (docs/features/36 Z6 "Test and
    # acceptance"): the same host_panel_zwave_* calls the button group makes, run head to tail
    # and checked against the panel's real state. Not part of the host test suite (that only
    # discovers components/<tier>/<name>/test); run it by hand after building with -ZWave. CI
    # runs the same program from host/ui_preview (ZW-021).
    add_executable(zwave_smoke ${PREVIEW}/zwave_smoke.c ${PREVIEW}/host_panel.c ${ZB_HOST}
                               ${ZWAVE_TEST}/fake_zac93.c ${ZWAVE_TEST}/fake_zdev.c
                               ${ZWAVE_TEST}/fake_zdev_cc.c ${ZWAVE_TEST}/zw_host_crypto.c)
    target_include_directories(zwave_smoke PRIVATE ${PREVIEW} ${ZWAVE_TEST})
    target_link_libraries(zwave_smoke PRIVATE alarm_ui alarm_proto sounder zwave_host zwave_proto
                                              zwave_zones zigbee_zones_fake)
    target_compile_definitions(zwave_smoke PRIVATE ALARM_SIM_ZWAVE=1 ${letters_dir})
    target_compile_options(zwave_smoke PRIVATE -Wall -Wextra -Werror)
    alarm_host_voice_pack(zwave_smoke)
endif()
target_compile_options(ui_sim PRIVATE -Wall -Wextra -Werror)
