# Every siren's policy, the wired outputs and the 433 MHz sirens (docs/features/16 OUT-002..OUT-015).
alarm_add_test(sirens SOURCES test_sirens.c LIBS sirens rf_decode alarm_core unity STRICT)
