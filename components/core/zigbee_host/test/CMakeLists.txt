# Getting the sensor radio running by itself (ZB-001, docs/features/20): the panel carries the
# ESP32-H2's firmware and puts it there, over the one UART the socket wires.
alarm_add_test(zb_bringup SOURCES test_zb_bringup.c LIBS zigbee_host unity)
