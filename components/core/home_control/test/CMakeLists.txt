# Devices (docs/features/24): the registry and its merge, the security classes, actions and their
# results, Home Assistant and the panel's Z-Wave outputs as device sources. No alarm engine is involved.
alarm_add_test(home_control SOURCES test_home_control.c LIBS home_control unity)
alarm_add_test(dsrc_zw SOURCES test_dsrc_zw.c LIBS home_control unity)
alarm_add_test(dsrc_ha SOURCES test_dsrc_ha.c LIBS home_control unity)
alarm_add_test(hc_home SOURCES test_hc_home.c LIBS home_control unity STRICT)
