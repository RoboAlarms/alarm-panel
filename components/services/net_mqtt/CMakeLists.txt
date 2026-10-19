# net_mqtt - the panel's MQTT client (docs/features/10): TLS, availability, retained state, events
# with store-and-forward from the event log, commands and Home Assistant discovery.
idf_component_register(SRCS "src/net_mqtt.c"
                       INCLUDE_DIRS "include"
                       REQUIRES alarm_proto platform_esp
                       PRIV_REQUIRES alarm_mem console i18n esp_app_format esp_timer esp-tls mbedtls
                                     mqtt nvs_flash)
