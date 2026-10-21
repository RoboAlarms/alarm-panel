# ui_port_esp - the touch UI's backend on ESP-IDF: ui_backend_t filled from the panel's services
# (docs/features/33 step 5). The UI itself (components/ui/alarm_ui) never sees a service.
idf_component_register(SRCS "src/ui_port_esp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ui_backend
                       PRIV_REQUIRES alarm_core alarm_proto automation_service platform_esp device_service
                                     ha_link lang_packs panel_voice
                                     net_mqtt net_notify nvs_flash outputs_service panel_clock panel_ota
                                     panel_storage rf_service zigbee_service zwave_service)
