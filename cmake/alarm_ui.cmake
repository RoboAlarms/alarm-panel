# The touch UI for the PC builds (UI preview and simulator): the generated LVGL Pro layout,
# the portable UI logic and its pictures - the same files the firmware compiles, listed once
# in components/ui/alarm_ui/sources.cmake.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/alarm_host.cmake)

set(ALARM_UI_ALL_FEATURES NOTIFY INTEGRATIONS STORAGE PHOTO_SAVER ZWAVE DEVICES AUTOMATIONS)

# alarm_add_ui_library(LAYOUT <WxH> [TARGET <name>] [FEATURES <NOTIFY|INTEGRATIONS|STORAGE|
#                      PHOTO_SAVER|ZWAVE|DEVICES|AUTOMATIONS>...] [DEFINITIONS <defs...>])
#   A static library (default name "alarm_ui") built from components/ui/alarm_ui/layouts/<WxH>/alarm_ui and
#   components/ui/alarm_ui with the optional pages FEATURES names (the same choices a
#   firmware's feature options make). Needs the "lvgl" target and the portable components.
function(alarm_add_ui_library)
    cmake_parse_arguments(ARG "" "LAYOUT;TARGET" "FEATURES;DEFINITIONS" ${ARGN})
    if(NOT ARG_TARGET)
        set(ARG_TARGET alarm_ui)
    endif()
    set(component ${ALARM_COMPONENTS_DIR}/ui/alarm_ui)
    set(layout ${component}/layouts/${ARG_LAYOUT}/alarm_ui)
    if(NOT EXISTS ${layout}/file_list_gen.cmake)
        message(FATAL_ERROR "No UI layout at ${layout}")
    endif()
    foreach(feature ${ALARM_UI_ALL_FEATURES})
        set(ALARM_UI_WITH_${feature} OFF)
    endforeach()
    foreach(feature ${ARG_FEATURES})
        if(NOT feature IN_LIST ALARM_UI_ALL_FEATURES)
            message(FATAL_ERROR "Unknown UI feature ${feature}")
        endif()
        set(ALARM_UI_WITH_${feature} ON)
    endforeach()

    set(LV_EDITOR_PROJECT_SOURCES)
    include(${layout}/file_list_gen.cmake)
    include(${component}/sources.cmake)
    set(logic ${ALARM_UI_LOGIC_SRCS})
    set(images ${ALARM_UI_IMAGE_SRCS})
    list(TRANSFORM logic PREPEND ${component}/)
    list(TRANSFORM images PREPEND ${component}/)

    add_library(${ARG_TARGET} STATIC ${LV_EDITOR_PROJECT_SOURCES} ${logic} ${images})
    target_include_directories(${ARG_TARGET} PUBLIC ${layout} ${component}/include
                               PRIVATE ${component}/src)
    target_link_libraries(${ARG_TARGET} PUBLIC lvgl ui_backend alarm_core event_log zone_hub i18n
                                               notify storage)
    target_compile_definitions(${ARG_TARGET} PRIVATE UI_SUBJECT_STRING_LENGTH=128
                                                     ${ALARM_UI_DEFINITIONS})
    if(ARG_DEFINITIONS)
        target_compile_definitions(${ARG_TARGET} PUBLIC ${ARG_DEFINITIONS})
    endif()
    set_source_files_properties(${logic} PROPERTIES COMPILE_OPTIONS "${ALARM_TEST_WARNINGS}")
endfunction()
