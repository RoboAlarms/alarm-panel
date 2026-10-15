# The CrowPanel Advanced ESP32-P4 family library: the board contract (components/boards/board_api)
# for every board built on Elecrow's ESP32-P4 main board - power, backlight, the MIPI-DSI display
# under LVGL, GT911 touch, audio, the STC battery helper. Each board of the family
# (board_crowpanel_p4, board_crowpanel_10) adds its description and its wiring and requires this.
idf_component_register(SRCS "src/board.c" "src/board_display.c" "src/board_touch.c"
                            "src/board_audio.c" "src/board_lvgl_memory.c" "src/board_battery.c"
                            "src/board_display_diag.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board_api board_info esp_lcd esp_driver_i2c lvgl__lvgl power_monitor
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_i2s esp_driver_tsens esp_hw_support
                                     esp_mm
                                     esp_timer
                                     espressif__esp_codec_dev
                                     espressif__esp_lcd_ek79007 espressif__esp_lcd_touch_gt911
                                     espressif__esp_lvgl_port)

# LVGL is linked after this board archive. Pull the custom allocator object in even though
# only LVGL references its functions (IDF 6 does not put every component in a link group).
if(CONFIG_LV_USE_CUSTOM_MALLOC)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=lv_malloc_core")
endif()
