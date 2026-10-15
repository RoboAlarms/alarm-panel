# The CrowPanel Advanced P4 application, shared by every board on Elecrow's ESP32-P4 main board:
# the 7" (this project) and the boards built on it (ALARM_BOARD_BASE crowpanel_p4, e.g.
# firmware/crowpanel_10). It is the shared start-up (panel_app) with the family's hooks
# (app_main.c) and the remote keypad through the P4's JPEG and H.264 encoders and its PPA
# (dev_adapter.c), which also hands the developer link Z-Wave when the build has it. The
# components are what these boards carry; each optional one follows its feature option
# (components/services/alarm_features).
#
#   include(<this file>)
#   crowpanel_p4_app(board_<board>)   # in the project's main/CMakeLists.txt
set(CROWPANEL_P4_APP_DIR ${CMAKE_CURRENT_LIST_DIR})

macro(crowpanel_p4_app board_component)
    idf_component_register(SRCS ${CROWPANEL_P4_APP_DIR}/app_main.c ${CROWPANEL_P4_APP_DIR}/dev_adapter.c
                           INCLUDE_DIRS ${CROWPANEL_P4_APP_DIR}
                           REQUIRES panel_app alarm_core alarm_features alarm_mem alarm_ui ui_port_esp
                                    ${board_component} ha_link net_mqtt net_notify panel_ota
                                    panel_storage platform_esp power_monitor rf_service zigbee_service
                                    device_service zone_hub console esp_event esp_netif esp_partition
                                    esp_timer nvs_flash
                                    dev_remote panel_web esp_driver_jpeg esp_driver_ppa
                                    espressif__esp_h264 esp_mm h264_sps zwave_service)
endmacro()
