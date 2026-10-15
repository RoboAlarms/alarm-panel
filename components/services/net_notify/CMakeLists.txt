idf_component_register(SRCS "src/net_notify.c"
                       INCLUDE_DIRS "include"
                       REQUIRES platform_esp
                       PRIV_REQUIRES alarm_mem alarm_proto console i18n ui_backend esp_http_client esp-tls esp_timer
                                     notify nvs_flash)
