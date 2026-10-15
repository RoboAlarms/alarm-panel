# panel_storage - the optional microSD card (mount, check, format on a worker task) and the flash
# accounting of Settings > About > Storage (docs/features/29). Optional: ALARM_FEATURE_STORAGE; off, it
# answers with stubs and the UI hides the Storage page. The SD wiring comes from board_info:
# SDMMC 1-bit (the P4) or SPI (the classic ESP32 boards).
idf_component_register(SRCS "src/panel_storage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board_info storage
                       PRIV_REQUIRES alarm_mem i18n platform_esp console fatfs sdmmc esp_driver_sdmmc
                                     esp_driver_sdspi esp_driver_spi
                                     spi_flash app_update bootloader_support esp_partition)
