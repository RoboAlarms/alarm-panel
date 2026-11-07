# server_link - the panel's link to an alarm-server (BL-225): MQTT 5 over a secure WebSocket, signed
# in with the panel's own Ed25519 key. The messages and rules live in alarm_proto (host-tested).
idf_component_register(SRCS "src/server_link.c" "src/phones.c" "src/app_push.c"
                       INCLUDE_DIRS "include"
                       REQUIRES alarm_proto platform_esp
                       PRIV_REQUIRES alarm_core alarm_mem console esp_app_format esp_timer esp-tls
                                     esp_http_client mbedtls mqtt notify nvs_flash panel_clock rtc_clock)
