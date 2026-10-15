# board_api - the functions every board component implements (docs/features/33 step 4, ADR-035).
# Header only; one board component per firmware provides the implementation.
idf_component_register(INCLUDE_DIRS "include"
                       REQUIRES board_info power_monitor lvgl__lvgl esp_common freertos
                                esp_driver_i2c)
