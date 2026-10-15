# The CrowPanel's description (board_desc.c) is safe for its chip and matches board_pins.h
# (BRD-008, docs/features/33). Compiles the board's portable description file, nothing else;
# board_pins.h includes the family's shared wiring (crowpanel_p4_pins.h).
set(board ${CMAKE_CURRENT_LIST_DIR}/..)
alarm_add_test(board_crowpanel_p4 SOURCES test_board_desc.c ${board}/src/board_desc.c
               LIBS board_info unity
               INCLUDES ${board}/src ${board}/include ${board}/../crowpanel_p4_family/include STRICT)
