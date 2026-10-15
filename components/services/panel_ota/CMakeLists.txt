# panel_ota - firmware updates for the panel and its Wi-Fi co-processor (docs/features/13): signed
# images, rollback self-test, the update server's checks and downloads, serial upload, install when idle.
# alarm_proto is public: panel_ota.h names proto_fw_language_t (the server's language packs).
idf_component_register(SRCS "src/panel_ota.c" "src/cp_ota.c" "src/ota_console.c" "src/ota_trust.c"
                       INCLUDE_DIRS "include"
                       REQUIRES alarm_core alarm_proto esp_common
                       PRIV_REQUIRES alarm_mem i18n ui_backend app_update bootloader_support console
                                     esp_app_format esp_driver_uart esp_http_client esp_partition
                                     mbedtls nvs_flash platform_esp)

# OTA-020: the key the installer accepted for the update server is tried behind ESP-IDF's own
# signature check, never instead of it (src/ota_trust.c). The wrap covers every caller in the
# image: esp_ota_end, esp_ota_set_boot_partition and the language packs' check.
if(CONFIG_SECURE_SIGNED_ON_UPDATE AND CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_secure_boot_verify_sbv2_signature_block")
endif()
