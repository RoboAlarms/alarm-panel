# Board support: Elecrow CrowPanel Advanced 7" (ESP32-P4), rev V1.1. The board contract
# (components/boards/board_api) comes from the CrowPanel Advanced P4 family library
# (components/boards/crowpanel_p4_family); this adds the board in data (board_desc.c: portable,
# validated by test/ on the PC), board_info() and board_name(), and its wiring (board_pins.h).
idf_component_register(SRCS "src/board_desc.c" "src/board_id.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board_api board_info crowpanel_p4_family)
