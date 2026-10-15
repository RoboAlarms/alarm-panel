idf_component_register(SRCS "src/dev_remote.c" "src/dev_cert.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common nvs_flash
    PRIV_REQUIRES alarm_mem alarm_core alarm_proto platform_esp panel_ota app_update console
    esp_app_format esp_https_server esp-tls esp_timer esp_hw_support mbedtls
    espressif__mdns zigbee_service panel_voice voice_pack lang_packs)
