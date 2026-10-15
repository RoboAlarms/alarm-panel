# The Z-Wave Serial API frame layer, CRC-16, DSK text, the Serial API's own commands and the
# command classes the panel speaks (docs/features/36, ZW-001, ZW-003, ZW-004, ZW-023).
alarm_add_test(zw_frame SOURCES test_zw_frame.c LIBS zwave_proto unity)
alarm_add_test(zw_api SOURCES test_zw_api.c LIBS zwave_proto unity)
alarm_add_test(zw_cc SOURCES test_zw_cc.c LIBS zwave_proto unity)
alarm_add_test(zw_entry SOURCES test_zw_entry.c LIBS zwave_proto unity)
alarm_add_test(zw_fuzz SOURCES test_zw_fuzz.c LIBS zwave_proto unity)
alarm_add_test(zw_redact SOURCES test_zw_redact.c LIBS zwave_proto unity)
alarm_add_test(zw_fwu SOURCES test_zw_fwu.c LIBS zwave_proto unity)
alarm_add_test(zw_switch SOURCES test_zw_switch.c LIBS zwave_proto unity)
alarm_add_test(zw_qr SOURCES test_zw_qr.c LIBS zwave_proto unity)
# A Gecko bootloader file checked, and the XMODEM-CRC blocks of the radio's update (ZW-025).
alarm_add_test(zw_gbl SOURCES test_zw_gbl.c LIBS zwave_proto unity)
