# Touch UI of the alarm panel: the LVGL Pro layout components/ui/alarm_ui/layouts/<WxH>/alarm_ui (generated C, fonts and the
# callbacks in alarm_ui.c), its logic (src/) and its pictures (images/). Portable: it reaches the
# panel only through ui_backend_t (components/core/ui_backend; on ESP-IDF the backend is
# components/services/ui_port_esp). The logic and pictures are listed once in sources.cmake,
# shared with the PC builds.
set(ALARM_UI_REQUIRES alarm_core event_log i18n lvgl__lvgl notify storage ui_backend zone_hub)

# A headless firmware (CONFIG_ALARM_FEATURE_UI off) compiles nothing here: only the headers stay,
# for callers that name the UI behind the same option. Requirements are read before the
# configuration exists, so they are the same either way.
if(DEFINED CONFIG_ALARM_FEATURE_UI AND NOT CONFIG_ALARM_FEATURE_UI)
    idf_component_register(INCLUDE_DIRS "include" REQUIRES ${ALARM_UI_REQUIRES})
    return()
endif()

# The layout the board's manifest names (CONFIG_ALARM_UI_LAYOUT, Kconfig here). Requirements are
# read before the configuration exists, so the default stands in until then.
set(ALARM_UI_LAYOUT "1024x600")
if(DEFINED CONFIG_ALARM_UI_LAYOUT AND NOT "${CONFIG_ALARM_UI_LAYOUT}" STREQUAL "")
    set(ALARM_UI_LAYOUT "${CONFIG_ALARM_UI_LAYOUT}")
endif()
set(UI_PROJECT ${CMAKE_CURRENT_LIST_DIR}/layouts/${ALARM_UI_LAYOUT}/alarm_ui)
if(NOT EXISTS ${UI_PROJECT}/file_list_gen.cmake)
    message(FATAL_ERROR "CONFIG_ALARM_UI_LAYOUT=${ALARM_UI_LAYOUT}: no layout at ${UI_PROJECT}")
endif()
set(LV_EDITOR_PROJECT_SOURCES)
include(${UI_PROJECT}/file_list_gen.cmake)
# The optional pages follow the firmware's feature options (components/services/alarm_features).
set(ALARM_UI_WITH_NOTIFY ${CONFIG_ALARM_FEATURE_NOTIFY})
if(CONFIG_ALARM_FEATURE_MQTT OR CONFIG_ALARM_FEATURE_HA)
    set(ALARM_UI_WITH_INTEGRATIONS ON)
endif()
set(ALARM_UI_WITH_STORAGE ${CONFIG_ALARM_FEATURE_STORAGE})
set(ALARM_UI_WITH_PHOTO_SAVER ${CONFIG_ALARM_FEATURE_PHOTO_SAVER})
# The Z-Wave pages come with the Z-Wave zone source (docs/features/36 Z8's option; until it
# exists no firmware has them).
set(ALARM_UI_WITH_ZWAVE ${CONFIG_ALARM_ZSRC_ZWAVE})
# The Smart Home screen comes with the devices registry (docs/features/24 HCTL-003, HCTL-010).
set(ALARM_UI_WITH_DEVICES ${CONFIG_ALARM_HOME_CONTROL})
# Settings > Automations comes with the automations service (docs/features/41 AUT-008).
set(ALARM_UI_WITH_AUTOMATIONS ${CONFIG_ALARM_FEATURE_AUTOMATIONS})
# The Zigbee pages come with the Zigbee zone source (in every P4 image, ADR-047); without a Zigbee
# service running, its backend leaves zb_status NULL and they stay hidden.
set(ALARM_UI_WITH_ZIGBEE ${CONFIG_ALARM_ZSRC_ZIGBEE})
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(SRCS ${LV_EDITOR_PROJECT_SOURCES}
                            ${ALARM_UI_LOGIC_SRCS} ${ALARM_UI_IMAGE_SRCS}
                       INCLUDE_DIRS "include" ${UI_PROJECT}
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES ${ALARM_UI_REQUIRES})

# The generated headers include "lvgl.h" only with this set; string subjects need 128 bytes.
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_LVGL_H_INCLUDE_SIMPLE
                           PRIVATE UI_SUBJECT_STRING_LENGTH=128 ${ALARM_UI_DEFINITIONS})
