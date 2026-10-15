# panel_clock - the battery-backed clock module (docs/features/35): finds a DS3231 on the board's I2C
# connector at start-up, sets the panel's clock from it before anything is logged, writes it after
# every time-server sync and when a person sets the time, and follows it when the internet is
# away. Optional: ALARM_FEATURE_RTC; off, it answers "not in this firmware" (stubs), and without
# a module it changes nothing.
idf_component_register(SRCS "src/panel_clock.c"
                       INCLUDE_DIRS "include"
                       REQUIRES rtc_clock esp_driver_i2c
                       PRIV_REQUIRES board_info rtc_ds3231 platform_esp console esp_timer nvs_flash)
