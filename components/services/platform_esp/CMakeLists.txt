# ESP-IDF glue for alarm_core: alarm task, NVS store, code hashing, event log, preferences, the
# sounder's task, Wi-Fi/time/addresses, the connection check, system functions, the zones task
# (zone_service: the hub, simulator, wired and Home Assistant sources) and the console.
# Optional parts are their own components (docs/features/33 step 6): rf_service, zigbee_service,
# device_service, panel_storage.
idf_component_register(SRCS "src/alarm_service.c" "src/alarm_store.c" "src/code_hash.c" "src/sounder.c"
                            "src/net_check.c" "src/net_wifi.c" "src/panel_console.c" "src/panel_log.c"
                            "src/panel_prefs.c" "src/panel_system.c" "src/zone_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES alarm_core alarm_proto event_log esp_event esp_netif sounder zone_hub
                       PRIV_REQUIRES alarm_mem i18n ui_backend spi_flash app_update bootloader_support console
                                     esp_app_format esp_driver_gpio esp_driver_uart esp_partition
                                     esp_timer esp_wifi lwip mbedtls nvs_flash)
