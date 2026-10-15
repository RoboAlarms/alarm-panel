# rtc_ds3231 - a DS3231 real-time clock module on an I2C bus (docs/features/35): finding it and moving
# its registers. What the registers mean is components/core/rtc_clock's; the service that keeps
# the panel's time with it is components/services/panel_clock.
idf_component_register(SRCS "src/rtc_ds3231.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES rtc_clock)
