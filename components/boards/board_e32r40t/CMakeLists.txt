# Board support: LCDWiki 4.0" ESP32-32E display (E32R40T), docs/features/19. Implements the board
# contract (components/boards/board_api): the ST7796S display under LVGL, the XPT2046 touch
# (components/drivers/touch_xpt2046, filtered and calibrated by components/core/touch_calib),
# the backlight; board_desc.c is the board in data (portable, validated by test/ on the PC).
# Audio and the battery are docs/features/19 Phase 2.
idf_component_register(SRCS "src/board.c" "src/board_desc.c" "src/board_display.c" "src/board_touch.c"
                            "src/board_console.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board_api board_info esp_lcd lvgl__lvgl
                       PRIV_REQUIRES console esp_driver_gpio esp_driver_ledc esp_driver_spi
                                     esp_timer nvs_flash touch_calib touch_xpt2046
                                     espressif__esp_lcd_st7796 espressif__esp_lvgl_port)
