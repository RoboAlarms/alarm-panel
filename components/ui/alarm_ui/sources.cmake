# The UI logic and pictures, listed once for the ESP-IDF component (CMakeLists.txt here) and
# the PC builds (cmake/alarm_ui.cmake). Paths are relative to this folder.
#
# The optional pages follow the features the caller sets before including this file
# (ON/OFF; docs/features/33 step 6): ALARM_UI_WITH_NOTIFY (ntfy, Pushover, the QR code),
# ALARM_UI_WITH_INTEGRATIONS (the MQTT and Home Assistant pages), ALARM_UI_WITH_STORAGE
# (About > Storage), ALARM_UI_WITH_PHOTO_SAVER (the screen saver; its JPEG comes from the
# picture pack, and the PC backend embeds images/saver_forest_jpg.c itself), ALARM_UI_WITH_ZWAVE
# (the Z-Wave pages, docs/features/36), ALARM_UI_WITH_DEVICES (the Smart Home screen,
# docs/features/24), ALARM_UI_WITH_AUTOMATIONS (Settings > Automations, docs/features/41),
# ALARM_UI_WITH_ZIGBEE (the Zigbee pages, docs/features/20 devices.md §9: on
# unless the caller sets it OFF - Zigbee is in every P4 image, ADR-047). The same choices reach the
# C code as ALARM_UI_NOTIFY, ALARM_UI_INTEGRATIONS, ALARM_UI_STORAGE, ALARM_UI_PHOTO_SAVER,
# ALARM_UI_ZWAVE, ALARM_UI_DEVICES, ALARM_UI_AUTOMATIONS and ALARM_UI_ZIGBEE (0 or 1) in
# ALARM_UI_DEFINITIONS.
set(ALARM_UI_LOGIC_SRCS
    src/ui_app.c
    src/ui_forms.c
    src/ui_installer.c
    src/ui_lang.c
    src/ui_net.c
    src/ui_settings.c
    src/ui_sources.c
    src/ui_status.c
    src/ui_text.c
    src/ui_theme.c
    src/ui_tz.c
    src/ui_update.c
    src/ui_voice.c
    src/ui_walk.c
    src/ui_wizard.c
)
set(ALARM_UI_IMAGE_SRCS
    images/roboalarms_mark_a8.c
)
set(ALARM_UI_DEFINITIONS)
if(NOT DEFINED ALARM_UI_WITH_ZIGBEE)
    set(ALARM_UI_WITH_ZIGBEE ON)
endif()
foreach(feature NOTIFY INTEGRATIONS STORAGE PHOTO_SAVER ZWAVE DEVICES AUTOMATIONS ZIGBEE)
    if(ALARM_UI_WITH_${feature})
        list(APPEND ALARM_UI_DEFINITIONS ALARM_UI_${feature}=1)
    else()
        list(APPEND ALARM_UI_DEFINITIONS ALARM_UI_${feature}=0)
    endif()
endforeach()
if(ALARM_UI_WITH_INTEGRATIONS)
    list(APPEND ALARM_UI_LOGIC_SRCS src/ui_integrations.c)
endif()
if(ALARM_UI_WITH_NOTIFY)
    list(APPEND ALARM_UI_LOGIC_SRCS src/ui_notify.c)
endif()
if(ALARM_UI_WITH_STORAGE)
    list(APPEND ALARM_UI_LOGIC_SRCS src/ui_storage.c)
endif()
if(ALARM_UI_WITH_PHOTO_SAVER)
    list(APPEND ALARM_UI_LOGIC_SRCS src/ui_saver.c)
endif()
if(ALARM_UI_WITH_ZWAVE)
    list(APPEND ALARM_UI_LOGIC_SRCS src/ui_zwave.c)
endif()
if(ALARM_UI_WITH_DEVICES)
    list(APPEND ALARM_UI_LOGIC_SRCS src/ui_devices.c)
endif()
if(ALARM_UI_WITH_AUTOMATIONS)
    list(APPEND ALARM_UI_LOGIC_SRCS src/ui_automations.c src/ui_auto_edit.c src/ui_auto_warn.c)
endif()
if(ALARM_UI_WITH_ZIGBEE)
    list(APPEND ALARM_UI_LOGIC_SRCS src/ui_zigbee.c)
endif()
