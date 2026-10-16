# PC preview of the touch UI: the real alarm engine + the LVGL Pro UI + the UI logic, rendered
# headless into PNG screenshots of every state. Build and run: pwsh tools/ui_preview.ps1
#
# ALARM_BOARD picks the board profile (firmware/<board>/sdkconfig.defaults, docs/features/33 step 9):
# its layout, screen size, optional pages and LVGL memory, exactly as its firmware builds them.
# The scripted walk (main.c) visits the pages the board has (host_features.h).
#
# ALARM_SIM_ZWAVE (pwsh tools/ui_preview.ps1 -ZWave, its own build folder) adds the Z-Wave world
# of host_panel.c and the Z-Wave pages (docs/features/36 Z7): the walk takes the same shots first,
# with the world still switched off, then the Z-Wave shots after them.
cmake_minimum_required(VERSION 3.20)
project(ui_preview C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/alarm_host.cmake)
include(${ALARM_REPO}/cmake/alarm_lvgl.cmake)
include(${ALARM_REPO}/cmake/alarm_ui.cmake)
include(${ALARM_REPO}/cmake/alarm_board.cmake)

set(ALARM_BOARD crowpanel_p4 CACHE STRING "Board profile: firmware/<board>/sdkconfig.defaults")
# -Locale (tools/ui_preview.ps1): the walk in another language (docs/features/37). A test
# language (en-XA, ar-XB: I18N-024) comes from the catalogs tools/i18n.py pseudo --build writes
# (the script runs it first), a real one (es-US) from the committed catalogs, drafts included.
# The English walk shows what a released panel shows: no draft language (I18N-005), so no
# language step in the setup until a translation is reviewed.
set(ALARM_PREVIEW_LOCALE "" CACHE STRING "Language for the walk, empty for English")
if(ALARM_PREVIEW_LOCALE MATCHES "-X[A-Z]$")
    set(I18N_CATALOG_DIR ${ALARM_REPO}/build/i18n/pseudo)
    if(NOT EXISTS ${I18N_CATALOG_DIR}/i18n_catalogs_gen.c)
        message(FATAL_ERROR "No test languages: run python tools/i18n.py pseudo --build")
    endif()
endif()
alarm_board_profile(${ALARM_BOARD})
option(ALARM_SIM_ZWAVE "Add the Z-Wave world and pages to the preview" OFF)
set(ui_features ${ALARM_BOARD_UI_FEATURES})
if(ALARM_SIM_ZWAVE)
    list(APPEND ui_features ZWAVE)
endif()
message(STATUS "UI preview: board ${ALARM_BOARD}, ${ALARM_BOARD_LAYOUT}, pages: "
               "${ALARM_BOARD_UI_FEATURES}, LVGL budget ${ALARM_BOARD_LV_BUDGET_KB} KB (0 = not checked)")
# The letters pack's parts the layout commits (host_panel.c reads them as a loaded pack would
# hand them out; a layout without the folder has none): docs/features/39 asset-packs.md.
set(screen ALARM_SCREEN_W=${ALARM_BOARD_WIDTH} ALARM_SCREEN_H=${ALARM_BOARD_HEIGHT}
    ALARM_LETTERS_DIR="${ALARM_COMPONENTS_DIR}/ui/alarm_ui/layouts/${ALARM_BOARD_LAYOUT}/alarm_ui/fonts/letters")

alarm_add_lvgl(CONF_DIR ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(lvgl PUBLIC ALARM_LV_BUDGET_KB=${ALARM_BOARD_LV_BUDGET_KB}
                                       ALARM_LV_DPI=${ALARM_BOARD_LV_DPI}
                                       ${ALARM_BOARD_LV_DEFINITIONS})
alarm_add_core_components()
if(NOT ALARM_PREVIEW_LOCALE)
    target_compile_definitions(i18n PRIVATE I18N_NO_DRAFTS)
endif()
alarm_add_ui_library(LAYOUT ${ALARM_BOARD_LAYOUT} FEATURES ${ui_features}
                     DEFINITIONS ${ALARM_BOARD_DEFINITIONS})

# The screenshots' PNG encoder, on the C library's heap rather than the board's LVGL heap
# (shot_png.c). LVGL's own code: its warnings are LVGL's, so not -Werror here.
add_library(shot_png STATIC shot_png.c)
target_link_libraries(shot_png PUBLIC lvgl)

# host_zigbee_demo.c: the demo Zigbee backend the walk's BL-144/147/148 shots are drawn over;
# host_zigbee_real.c: the panel's real Zigbee over the fake radio (BL-143 ZB-M6), the walk's
# zb-real segment and the simulator's default; host_link_demo.c: the panel link's demo backend
# (BL-048), which host_panel.c wires the same way.
set(ZB_HOST host_zigbee_demo.c host_zigbee_real.c host_link_demo.c)
# voice_walk.c: the scripted voice check (what the policy says and the files it renders); it also
# compiles the simulator's audio core (host/ui_sim/sim_audio.c, no sound card here) to prove the
# simulator's speech path.
add_executable(ui_preview main.c voice_walk.c ../ui_sim/sim_audio.c host_panel.c ${ZB_HOST})
target_include_directories(ui_preview PRIVATE ../ui_sim)
target_link_libraries(ui_preview PRIVATE alarm_ui alarm_proto automation shot_png zigbee_zones_fake)
if(WIN32)
    target_link_libraries(ui_preview PRIVATE winmm) # sim_audio.c's waveOut, never opened here
endif()
target_compile_definitions(ui_preview PRIVATE ${screen})
target_compile_options(ui_preview PRIVATE -Wall -Wextra -Werror)
# The real voice, packed at build time (voice_pack.cmake); ALARM_PREVIEW_VOICE=none|loading|
# unavailable at run time shows the voice that way instead (host_panel.c).
include(${CMAKE_CURRENT_LIST_DIR}/voice_pack.cmake)
alarm_host_voice_pack(ui_preview)
if(ALARM_PREVIEW_LOCALE)
    target_compile_definitions(ui_preview PRIVATE ALARM_PREVIEW_LOCALE="${ALARM_PREVIEW_LOCALE}"
        ALARM_PREVIEW_POT="${ALARM_REPO}/components/core/i18n/po/messages.pot")
endif()
# -Live (tools/ui_preview.ps1): start in English, change to ALARM_PREVIEW_LOCALE while running.
option(ALARM_PREVIEW_LIVE "Change to the test language while running" OFF)
if(ALARM_PREVIEW_LIVE)
    target_compile_definitions(ui_preview PRIVATE ALARM_PREVIEW_LIVE=1)
endif()
if(ALARM_SIM_ZWAVE)
    # The fake ZAC93 and its simulated devices (test sources without a test framework), as
    # host/ui_sim builds them.
    set(ZWAVE_TEST ${ALARM_COMPONENTS_DIR}/core/zwave_host/test)
    target_sources(ui_preview PRIVATE ${ZWAVE_TEST}/fake_zac93.c ${ZWAVE_TEST}/fake_zdev.c
                                      ${ZWAVE_TEST}/fake_zdev_cc.c ${ZWAVE_TEST}/zw_host_crypto.c)
    target_include_directories(ui_preview PRIVATE ${ZWAVE_TEST})
    target_link_libraries(ui_preview PRIVATE zwave_host zwave_proto zwave_zones)
    target_compile_definitions(ui_preview PRIVATE ALARM_SIM_ZWAVE=1)

    # ZW-021: the scripted, windowless proof of the Z-Wave world (zwave_smoke.c), the same
    # program host/ui_sim builds on Windows. Linux CI has no ui_sim, so it is built and run here.
    add_executable(zwave_smoke zwave_smoke.c host_panel.c ${ZB_HOST}
                               ${ZWAVE_TEST}/fake_zac93.c ${ZWAVE_TEST}/fake_zdev.c
                               ${ZWAVE_TEST}/fake_zdev_cc.c ${ZWAVE_TEST}/zw_host_crypto.c)
    target_include_directories(zwave_smoke PRIVATE ${ZWAVE_TEST})
    target_link_libraries(zwave_smoke PRIVATE alarm_ui alarm_proto automation zwave_host zwave_proto
                                              zwave_zones zigbee_zones_fake)
    target_compile_definitions(zwave_smoke PRIVATE ${screen} ALARM_SIM_ZWAVE=1)
    target_compile_options(zwave_smoke PRIVATE -Wall -Wextra -Werror)
    alarm_host_voice_pack(zwave_smoke)
endif()

# The UI with every optional page left out, started against the same simulated panel
# (docs/features/33 step 6): the gated build links, starts and hides what is missing. No FEATURES:
# DEVICES and AUTOMATIONS (Settings > Automations, docs/features/41) among them are OFF.
set(ALARM_UI_WITH_ZIGBEE OFF) # on by default (sources.cmake): left out here too
alarm_add_ui_library(LAYOUT ${ALARM_BOARD_LAYOUT} TARGET alarm_ui_minimal)
unset(ALARM_UI_WITH_ZIGBEE)
add_executable(ui_smoke_minimal smoke_minimal.c host_panel.c ${ZB_HOST})
target_link_libraries(ui_smoke_minimal PRIVATE alarm_ui_minimal alarm_proto automation zigbee_zones_fake)
target_compile_definitions(ui_smoke_minimal PRIVATE ${screen})
target_compile_options(ui_smoke_minimal PRIVATE -Wall -Wextra -Werror)
alarm_host_voice_pack(ui_smoke_minimal)

# The words not drawn in a picture: ui_text.c and Home's headline, detail and note, read from the
# UI's subjects with the panel driven into each state (BL-066, KPD-004). Needs ui_text.h, which is
# the UI's own header.
add_executable(ui_words_test ui_words_test.c host_panel.c ${ZB_HOST})
target_link_libraries(ui_words_test PRIVATE alarm_ui alarm_proto automation zigbee_zones_fake)
target_include_directories(ui_words_test PRIVATE ${ALARM_COMPONENTS_DIR}/ui/alarm_ui/src)
target_compile_definitions(ui_words_test PRIVATE ${screen})
target_compile_options(ui_words_test PRIVATE -Wall -Wextra -Werror)
alarm_host_voice_pack(ui_words_test)

# The automations core in the simulator (docs/features/41 U12, BL-168): the real engine, observer
# and runner on the simulator's devices, with Harbor House's seeded automations (auto_smoke.c).
# Windowless; exits non-zero when a check fails.
add_executable(auto_smoke auto_smoke.c host_panel.c ${ZB_HOST})
target_link_libraries(auto_smoke PRIVATE alarm_ui alarm_proto automation zigbee_zones_fake)
target_compile_definitions(auto_smoke PRIVATE ${screen})
target_compile_options(auto_smoke PRIVATE -Wall -Wextra -Werror)
alarm_host_voice_pack(auto_smoke)
enable_testing()
add_test(NAME auto_smoke COMMAND auto_smoke)

# The voice backend (docs/features/08 voice-first-build.md, BL-202 U13a): the five voice hooks
# over the packed en-US voice and the host engine (voice_smoke.c). Windowless; skipped (77)
# without a pack.
add_executable(voice_smoke voice_smoke.c host_panel.c ${ZB_HOST})
target_link_libraries(voice_smoke PRIVATE alarm_ui alarm_proto automation zigbee_zones_fake)
target_compile_definitions(voice_smoke PRIVATE ${screen})
target_compile_options(voice_smoke PRIVATE -Wall -Wextra -Werror)
alarm_host_voice_pack(voice_smoke)
add_test(NAME voice_smoke COMMAND voice_smoke)
set_tests_properties(voice_smoke PROPERTIES SKIP_RETURN_CODE 77)

# The voice walk on its own (voice_walk.c, the check the preview's walk ends with): windowless and
# fast, with the simulator's audio core compiled without its sound card.
add_executable(voice_walk_smoke voice_walk_main.c voice_walk.c ../ui_sim/sim_audio.c host_panel.c
                                ${ZB_HOST})
target_include_directories(voice_walk_smoke PRIVATE ../ui_sim)
target_link_libraries(voice_walk_smoke PRIVATE alarm_ui alarm_proto automation zigbee_zones_fake)
if(WIN32)
    target_link_libraries(voice_walk_smoke PRIVATE winmm) # offline mode: no audio device
endif()
target_compile_definitions(voice_walk_smoke PRIVATE ${screen})
target_compile_options(voice_walk_smoke PRIVATE -Wall -Wextra -Werror)
alarm_host_voice_pack(voice_walk_smoke)
add_test(NAME voice_walk_smoke COMMAND voice_walk_smoke)
