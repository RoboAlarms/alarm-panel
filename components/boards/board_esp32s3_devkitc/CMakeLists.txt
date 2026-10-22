# Board support: ESP32-S3-DevKitC-1 (N16R8) as a headless panel, docs/features/42. Implements the
# board contract (components/boards/board_api) for a board with no screen, touch, speaker or
# battery; board_desc.c is the board in data (portable, validated by test/ on the PC).
idf_component_register(SRCS "src/board.c" "src/board_desc.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board_api board_info lvgl__lvgl
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c)
