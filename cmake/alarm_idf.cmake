# Included through alarm_firmware.cmake by every ESP-IDF firmware project, before ESP-IDF's
# project.cmake: where the project's components are.
#
# Each tier folder holds components (ADR-034); ESP-IDF searches one level below every entry
# of EXTRA_COMPONENT_DIRS, so the tiers are listed, not the components. A component's test/
# folder is two levels down and never becomes a component.
#
# The boards tier is the exception: a firmware sees the board contract and its own board only
# (ALARM_BOARD, the project's folder name), never another board's component. The component
# manager solves the registry dependencies of every component it can see, so a display driver
# made for another chip would otherwise stop the build. A board built on a family library
# (ALARM_BOARD_FAMILY: components/boards/<name>_family, what the boards on one main board share,
# e.g. crowpanel_p4_family) sees that library too.
include_guard(GLOBAL)

if(NOT ALARM_BOARD)
    message(FATAL_ERROR "Set ALARM_BOARD (firmware/<board>, components/boards/board_<board>) "
                        "before including alarm_idf.cmake")
endif()

get_filename_component(ALARM_REPO ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(ALARM_COMPONENT_TIERS core drivers boards services ui app)

set(EXTRA_COMPONENT_DIRS)
foreach(tier ${ALARM_COMPONENT_TIERS})
    if(tier STREQUAL "boards")
        list(APPEND EXTRA_COMPONENT_DIRS ${ALARM_REPO}/components/boards/board_api
                                         ${ALARM_REPO}/components/boards/board_${ALARM_BOARD})
        if(ALARM_BOARD_FAMILY)
            list(APPEND EXTRA_COMPONENT_DIRS ${ALARM_REPO}/components/boards/${ALARM_BOARD_FAMILY})
        endif()
    else()
        list(APPEND EXTRA_COMPONENT_DIRS ${ALARM_REPO}/components/${tier})
    endif()
endforeach()
