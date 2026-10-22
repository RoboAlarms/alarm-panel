# The DevKitC-1's description (board_desc.c) is safe for its chip and keeps the module's flash,
# PSRAM, USB and strapping pins out (BRD-008, docs/features/42). Compiles the board's portable
# description file, nothing else.
set(board ${CMAKE_CURRENT_LIST_DIR}/..)
alarm_add_test(board_esp32s3_devkitc SOURCES test_board_desc.c ${board}/src/board_desc.c
               LIBS board_info unity INCLUDES ${board}/src ${board}/include STRICT)
