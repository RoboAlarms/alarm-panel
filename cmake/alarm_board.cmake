# A board's profile for the PC builds (UI preview, simulator), read from the same manifest its
# firmware is built from: firmware/<board>/sdkconfig.defaults (docs/features/33 step 9), after its
# base's when the board is built on another board's project (ALARM_BOARD_BASE,
# cmake/alarm_firmware.cmake). The Python checker tools/check_boards.py parses the same lines the
# same way (a test compares them).
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/alarm_host.cmake)

# alarm_read_manifest(<files> <prefix>)
#   Sets <prefix>_<OPTION> for every "CONFIG_<OPTION>=value" (quotes dropped) and "n" for every
#   "# CONFIG_<OPTION> is not set" in the files, read in order; the last line for an option wins.
#   <prefix>_OPTIONS lists them.
function(alarm_read_manifest files prefix)
    set(lines)
    foreach(file IN LISTS files)
        if(NOT EXISTS ${file})
            message(FATAL_ERROR "No board manifest at ${file}")
        endif()
        file(STRINGS ${file} file_lines)
        list(APPEND lines ${file_lines})
    endforeach()
    set(options)
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line MATCHES "^CONFIG_([A-Z0-9_]+)=(.*)$")
            set(option ${CMAKE_MATCH_1})
            string(STRIP "${CMAKE_MATCH_2}" value)
            if(value MATCHES "^\"(.*)\"$")
                set(value "${CMAKE_MATCH_1}")
            endif()
        elseif(line MATCHES "^#[ \t]*CONFIG_([A-Z0-9_]+) is not set$")
            set(option ${CMAKE_MATCH_1})
            set(value "n")
        else()
            continue()
        endif()
        set(${prefix}_${option} "${value}" PARENT_SCOPE)
        list(APPEND options ${option})
    endforeach()
    list(REMOVE_DUPLICATES options)
    set(${prefix}_OPTIONS ${options} PARENT_SCOPE)
endfunction()

# alarm_board_manifests(<board> <out>)
#   The files of the board's manifest, in the order the build reads them: firmware/<base>/
#   sdkconfig.defaults when firmware/<board>/CMakeLists.txt has set(ALARM_BOARD_BASE <base>),
#   then firmware/<board>/sdkconfig.defaults.
function(alarm_board_manifests board out)
    set(dir ${ALARM_REPO}/firmware/${board})
    set(files)
    if(EXISTS ${dir}/CMakeLists.txt)
        file(STRINGS ${dir}/CMakeLists.txt base_line
             REGEX "^[ \t]*set\\(ALARM_BOARD_BASE[ \t]+[a-z0-9_]+[ \t]*\\)")
        if(base_line MATCHES "ALARM_BOARD_BASE[ \t]+([a-z0-9_]+)")
            list(APPEND files ${ALARM_REPO}/firmware/${CMAKE_MATCH_1}/sdkconfig.defaults)
        endif()
    endif()
    list(APPEND files ${dir}/sdkconfig.defaults)
    set(${out} ${files} PARENT_SCOPE)
endfunction()

# alarm_board_profile(<board>)
#   From the board's manifest (alarm_board_manifests):
#     ALARM_BOARD_LAYOUT       the UI layout (components/ui/alarm_ui/layouts/<WxH>/alarm_ui)
#     ALARM_BOARD_WIDTH/HEIGHT the screen, from the layout's name
#     ALARM_BOARD_UI_FEATURES  the optional UI pages the firmware builds (alarm_add_ui_library)
#     ALARM_BOARD_LV_BUDGET_KB what LVGL may take for the UI (CONFIG_ALARM_UI_LVGL_BUDGET_KB),
#                              0 = not checked; the preview reports its peak against it
#     ALARM_BOARD_LV_DPI       LVGL's DPI (CONFIG_LV_DPI_DEF), else 0 (LVGL's default)
#     ALARM_BOARD_DEFINITIONS  the board's optional parts as CONFIG_<option>=0|1, as
#                              sdkconfig.h gives them to its firmware: the UI reads the
#                              remote control hooks, the simulated panel offers what the
#                              firmware's backend would, the walk visits the board's pages
#                              (host/ui_preview/host_features.h)
#     ALARM_BOARD_LV_DEFINITIONS the LVGL parts the manifest states (widgets, layouts, draw
#                              formats, the JPEG and QR code libraries, compressed fonts) as
#                              LV_<option>=0|1 for the PC's LVGL: a widget the board lacks
#                              fails the PC build as it fails the firmware's, and a draw format
#                              it lacks is missing from the screenshots as from the screen
set(ALARM_LV_MANIFEST_PARTS
    ANIMIMG ARC ARCLABEL BAR BUTTON BUTTONMATRIX CALENDAR CANVAS CHART CHECKBOX DROPDOWN IMAGE
    IMAGEBUTTON KEYBOARD LABEL LED LINE LIST LOTTIE MENU MSGBOX ROLLER SCALE SLIDER SPAN SPINBOX
    SPINNER SWITCH TABLE TABVIEW TEXTAREA TILEVIEW WIN FLEX GRID TJPGD QRCODE FONT_COMPRESSED)
function(alarm_board_profile board)
    alarm_board_manifests(${board} manifests)
    alarm_read_manifest("${manifests}" M)
    if(NOT M_ALARM_FEATURE_UI STREQUAL "y")
        message(FATAL_ERROR "${board}: this firmware has no touch UI (CONFIG_ALARM_FEATURE_UI is "
                            "off in its manifest), so there is nothing to preview or simulate")
    endif()
    if(NOT M_ALARM_UI_LAYOUT MATCHES "^([0-9]+)x([0-9]+)$")
        message(FATAL_ERROR "${board}: CONFIG_ALARM_UI_LAYOUT '${M_ALARM_UI_LAYOUT}' is not <W>x<H>")
    endif()
    set(ALARM_BOARD_LAYOUT ${M_ALARM_UI_LAYOUT} PARENT_SCOPE)
    set(ALARM_BOARD_WIDTH ${CMAKE_MATCH_1} PARENT_SCOPE)
    set(ALARM_BOARD_HEIGHT ${CMAKE_MATCH_2} PARENT_SCOPE)

    set(features)
    if(M_ALARM_FEATURE_NOTIFY STREQUAL "y")
        list(APPEND features NOTIFY)
    endif()
    if(M_ALARM_FEATURE_MQTT STREQUAL "y" OR M_ALARM_FEATURE_HA STREQUAL "y")
        list(APPEND features INTEGRATIONS)
    endif()
    if(M_ALARM_FEATURE_STORAGE STREQUAL "y")
        list(APPEND features STORAGE)
    endif()
    if(M_ALARM_FEATURE_PHOTO_SAVER STREQUAL "y")
        list(APPEND features PHOTO_SAVER)
    endif()
    if(M_ALARM_HOME_CONTROL STREQUAL "y")
        list(APPEND features DEVICES)
    endif()
    if(M_ALARM_FEATURE_AUTOMATIONS STREQUAL "y")
        list(APPEND features AUTOMATIONS)
    endif()
    set(ALARM_BOARD_UI_FEATURES ${features} PARENT_SCOPE)

    set(budget 0)
    if(DEFINED M_ALARM_UI_LVGL_BUDGET_KB)
        set(budget ${M_ALARM_UI_LVGL_BUDGET_KB})
    endif()
    set(ALARM_BOARD_LV_BUDGET_KB ${budget} PARENT_SCOPE)
    set(lv_dpi 0)
    if(DEFINED M_LV_DPI_DEF)
        set(lv_dpi ${M_LV_DPI_DEF})
    endif()
    set(ALARM_BOARD_LV_DPI ${lv_dpi} PARENT_SCOPE)

    set(defs)
    foreach(option ALARM_FEATURE_MQTT ALARM_FEATURE_NOTIFY ALARM_FEATURE_HA
                   ALARM_FEATURE_STORAGE ALARM_FEATURE_PHOTO_SAVER ALARM_PANEL_WEB
                   ALARM_DEV_REMOTE ALARM_HOME_CONTROL ALARM_FEATURE_AUTOMATIONS)
        if(M_${option} STREQUAL "y")
            list(APPEND defs CONFIG_${option}=1)
        else()
            list(APPEND defs CONFIG_${option}=0)
        endif()
    endforeach()
    set(ALARM_BOARD_DEFINITIONS ${defs} PARENT_SCOPE)

    set(lv_defs)
    foreach(option IN LISTS M_OPTIONS)
        set(part OFF)
        if(option MATCHES "^LV_DRAW_SW_SUPPORT_[A-Z0-9_]+$")
            set(part ON)
        elseif(option MATCHES "^LV_USE_([A-Z0-9_]+)$")
            # list(FIND), not if(IN_LIST): run with `cmake -P` (tools/tests/test_check_boards.py),
            # this file has no cmake_minimum_required, and CMake 3 then refuses IN_LIST (CMP0057).
            list(FIND ALARM_LV_MANIFEST_PARTS "${CMAKE_MATCH_1}" part_at)
            if(NOT part_at EQUAL -1)
                set(part ON)
            endif()
        endif()
        if(part AND M_${option} STREQUAL "y")
            list(APPEND lv_defs ${option}=1)
        elseif(part)
            list(APPEND lv_defs ${option}=0)
        endif()
    endforeach()
    set(ALARM_BOARD_LV_DEFINITIONS ${lv_defs} PARENT_SCOPE)
endfunction()
