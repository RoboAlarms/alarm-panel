# Board descriptions and their validation (BRD-NNN, docs/features/33 step 4): the chips' pin facts,
# and every way a description can harm a board being refused.
alarm_add_test(board_info SOURCES test_board_info.c LIBS board_info unity STRICT)
