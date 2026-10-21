# panel_app - the panel's start-up, shared by every board (docs/features/33 step 7). The app tier:
# it composes the services, the UI and the board contract, and names no board.
idf_component_register(SRCS "src/panel_app.c"
                       INCLUDE_DIRS "include"
                       REQUIRES alarm_features power_monitor ui_backend
                       PRIV_REQUIRES alarm_core alarm_mem alarm_ui automation_service backup_service board_api device_service ha_link
                                     lang_packs net_mqtt net_notify outputs_service panel_clock panel_ota panel_storage panel_voice platform_esp
                                     plink_service
                                     rf_service ui_port_esp zigbee_service zone_hub zwave_service console
                                     esp_driver_spi esp_event esp_netif esp_partition esp_timer
                                     lvgl__lvgl nvs_flash)
