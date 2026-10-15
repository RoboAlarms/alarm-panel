# rf_service - the 433 MHz receiver as a zone source (docs/features/25): runs the CC1101 or a receiver
# module (components/drivers/rf_rx), decodes (rf_decode) and feeds zsrc_rf through the zones
# task. Optional: ALARM_ZSRC_RF; off, it answers "not in this firmware" (stubs).
idf_component_register(SRCS "src/rf_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_spi zone_hub rf_decode
                       PRIV_REQUIRES alarm_mem board_info platform_esp rf_rx cc1101_regs console esp_timer
                                     nvs_flash)
