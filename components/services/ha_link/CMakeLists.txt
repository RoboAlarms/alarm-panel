# ha_link - the local Home Assistant link (docs/features/23, M12): mDNS, the TLS server on port
# 6054, pairing on the panel's screen. The protocol itself lives in alarm_proto (host-tested).
idf_component_register(SRCS "src/ha_link.c" "src/ha_cert.c"
                       INCLUDE_DIRS "include"
                       REQUIRES platform_esp
                       PRIV_REQUIRES alarm_mem alarm_core alarm_proto i18n console esp_app_format lwip mbedtls
                                     device_service net_mqtt nvs_flash panel_ota)
