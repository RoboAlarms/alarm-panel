# Z-Wave devices as zones end to end: zw_net against zwave_host's fake ZAC93 and simulated devices,
# the bridge, the zone source and the real hub (docs/features/36 Z6; ZW-013, ZW-014, ZW-015).
set(ZWAVE_HOST_TEST ${CMAKE_CURRENT_SOURCE_DIR}/../../zwave_host/test)
alarm_add_test(zw_keypad SOURCES test_zw_keypad.c LIBS zwave_zones alarm_sim)
alarm_add_test(zw_feedback SOURCES test_zw_feedback.c LIBS zwave_zones alarm_sim)
alarm_add_test(zw_keyctl SOURCES test_zw_keyctl.c LIBS zwave_zones alarm_sim)
# Which firmware the Z-Wave JS service's answer offers a device (docs/features/36 ZW-033).
alarm_add_test(zw_fwcat SOURCES test_zw_fwcat.c LIBS zwave_zones unity)
alarm_add_test(zw_fwjob SOURCES test_zw_fwjob.c LIBS zwave_zones unity)
# What the devices switch, and a garage door (docs/features/36 ZW-034, ZW-035).
alarm_add_test(zw_outputs SOURCES test_zw_outputs.c LIBS zwave_zones unity)
# Z-Wave sirens as the panel's bell (docs/features/16 OUT-016..OUT-019, docs/features/36 sirens.md).
alarm_add_test(zw_siren SOURCES test_zw_siren.c LIBS zwave_zones sirens unity STRICT)
alarm_add_test(zwave_zones SOURCES test_zwave_zones.c ${ZWAVE_HOST_TEST}/fake_zac93.c
               ${ZWAVE_HOST_TEST}/fake_zdev.c ${ZWAVE_HOST_TEST}/fake_zdev_cc.c
               ${ZWAVE_HOST_TEST}/zw_host_crypto.c
               LIBS zwave_zones zwave_host zwave_proto zone_hub unity
               INCLUDES ${ZWAVE_HOST_TEST})
# The panel's Z-Wave as zwave_service runs it: the pages' view and requests, the logged events and
# what is stored, against the same fake module and devices (docs/features/36 Z8; ZW-005, ZW-016,
# ZW-017, ZW-018, ZW-019, ZW-022).
alarm_add_test(zwz_panel SOURCES test_zwz_panel.c ${ZWAVE_HOST_TEST}/fake_zac93.c
               ${ZWAVE_HOST_TEST}/fake_zdev.c ${ZWAVE_HOST_TEST}/fake_zdev_cc.c
               ${ZWAVE_HOST_TEST}/zw_host_crypto.c
               LIBS zwave_zones zwave_host zwave_proto zone_hub ui_backend unity
               INCLUDES ${ZWAVE_HOST_TEST})
# The stored blobs, the Security 2 receive path and the bridge against random and mutated input,
# with fixed seeds; real crypto with fixed keys (docs/features/36 ZW-023, ZW-009).
alarm_add_test(zwz_fuzz SOURCES test_zwz_fuzz.c ${ZWAVE_HOST_TEST}/fake_zac93.c
               ${ZWAVE_HOST_TEST}/fake_zdev.c ${ZWAVE_HOST_TEST}/fake_zdev_cc.c
               ${ZWAVE_HOST_TEST}/zw_host_crypto.c
               LIBS zwave_zones zwave_host zwave_proto zone_hub ui_backend unity
               INCLUDES ${ZWAVE_HOST_TEST})
# What zwz_panel says changed, into NVS "zwave": zwz_store_run, the one copy of zwave_service's
# persist() and the simulator's, against a fake NVS that refuses chosen keys (docs/features/36
# network.md §2; ZW-017, ZW-039).
alarm_add_test(zwz_store SOURCES test_zwz_store.c ${ZWAVE_HOST_TEST}/zw_host_crypto.c
               LIBS zwave_zones zwave_host zwave_proto zone_hub ui_backend unity
               INCLUDES ${ZWAVE_HOST_TEST})
# Recorded exchanges with the ZAC93 as tests: zwtrace recordings (panelctl's `zwave check` on the
# bench, or the fake's through the recorder) replayed into zwz_panel, the pages' view read after
# (docs/features/36 "Test and acceptance", traces/README.md; ZW-004, ZW-005, ZW-016). Every
# traces/*.zwtrace is replayed; a new one is found at the next build.
alarm_add_test(zwz_replay SOURCES test_zwz_replay.c zwtrace.c zwtrace_record.c zwtrace_replay.c
               ${ZWAVE_HOST_TEST}/fake_zac93.c ${ZWAVE_HOST_TEST}/fake_zdev.c
               ${ZWAVE_HOST_TEST}/fake_zdev_cc.c ${ZWAVE_HOST_TEST}/zw_host_crypto.c
               LIBS zwave_zones zwave_host zwave_proto zone_hub ui_backend unity
               INCLUDES ${ZWAVE_HOST_TEST})
file(GLOB ZWZ_TRACES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/traces
     ${CMAKE_CURRENT_SOURCE_DIR}/traces/*.zwtrace)
list(SORT ZWZ_TRACES)
list(JOIN ZWZ_TRACES "," ZWZ_TRACE_LIST)
target_compile_definitions(test_zwz_replay PRIVATE
    ZWZ_TRACE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/traces"
    ZWZ_TRACE_FILES="${ZWZ_TRACE_LIST}")
