# touch_xpt2046 - the XPT2046 resistive touch controller on an SPI bus (docs/features/19): raw
# samples as a touch_raw_t; calibration and filtering are components/core/touch_calib.
idf_component_register(SRCS "src/touch_xpt2046.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_spi touch_calib
                       PRIV_REQUIRES freertos)
