# rf_rx - the panel's sub-GHz receiver (docs/features/25): a TI CC1101 on SPI, run as an OOK receiver
# whose demodulated line (GDO0) the RMT peripheral turns into pulse widths. Only the hardware
# lives here (and the 433 MHz sirens' transmissions, rf_tx.c); the pulses go to components/core/rf_decode and the codes to zone_hub's zsrc_rf.
idf_component_register(SRCS "src/cc1101.c" "src/rf_capture.c" "src/rf_tx.c"
                       INCLUDE_DIRS "include"
                       REQUIRES cc1101_regs esp_driver_spi esp_driver_rmt esp_driver_gpio
                       PRIV_REQUIRES esp_timer)
