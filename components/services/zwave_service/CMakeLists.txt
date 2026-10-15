# zwave_service - Z-Wave sensors through a Zooz ZAC93 (docs/features/36 Z8): the zw_link task (the
# UART, framing, ACK/NAK), the zwave task (zwave_zones' zwz_panel: the network, the bridge to the
# zone source, the pages' requests), NVS "zwave", the crypto port on PSA, the `zwave` console
# command, the touch UI's zw_* calls and the developer link's JSON with, in development builds,
# the link's frame trace (docs/features/30). Optional: ALARM_ZSRC_ZWAVE; off, every source compiles
# to nothing and callers guard their calls (the portable libraries it requires are then built
# but never linked).
idf_component_register(SRCS "src/zwave_service.c" "src/zw_link.c" "src/zw_psa.c"
                            "src/zw_trace.c" "src/zwave_console.c" "src/zwave_dev.c"
                            "src/zw_fwsvc.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common ui_backend
                       PRIV_REQUIRES alarm_core alarm_mem alarm_proto board_info console esp_app_format
                                     esp_driver_gpio esp_driver_uart esp_http_client esp_timer mbedtls nvs_flash
                                     platform_esp zone_hub zwave_host zwave_proto zwave_zones)
