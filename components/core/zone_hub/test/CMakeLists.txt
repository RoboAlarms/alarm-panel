# Zone sources (ZSRC-NNN, docs/features/22): bindings, mapping, grace and CHECK, the binding blob,
# the debouncer and the simulator, end to end with the engine; the 433 MHz source (RF-NNN,
# docs/features/25): learning codes, holds, repeats, jamming; the Zigbee source (docs/features/20): IAS
# enrollment and status mapping, supervision, jamming and the quirks table.
alarm_add_test(zone_hub SOURCES test_zone_hub.c LIBS zone_hub alarm_sim)
alarm_add_test(rf SOURCES test_rf.c LIBS zone_hub alarm_sim rf_decode)
# 433 MHz products as mock devices (rf_mock.c, docs/features/25 "Mock devices"): their own timings,
# codes and habits, through a model of the air and the receiver into the real decoder and source.
alarm_add_test(rf_devices SOURCES test_rf_devices.c rf_mock.c LIBS zone_hub alarm_sim rf_decode)
alarm_add_test(zigbee SOURCES test_zigbee.c LIBS zone_hub alarm_sim)
# The Z-Wave source (docs/features/36 zones.md): addresses and generations, the inputs a device
# offers, every report row, holds, check-ins and grace, trust per device, fresh state, the link
# and the quirks (ZW-013, ZW-014, ZW-015).
alarm_add_test(zsrc_zwave SOURCES test_zsrc_zwave.c LIBS zone_hub alarm_sim)
# Zigbee device signatures (docs/features/20 testing.md §2): each row of
# components/core/zigbee_zones/test/signatures (tools/zb_signatures.py) through the source as the
# radio side feeds it (ZB-003, ZB-005, ZB-007, ZB-012).
alarm_add_test(zigbee_signatures SOURCES test_zb_signatures.c LIBS zone_hub alarm_sim)
# Spoken-name settling (docs/features/08 voice-first-build.md D07, CORE-CFG-021): which zones'
# changed sensors still owe an invalidation and which are settling, by source-change epoch.
alarm_add_test(zone_voice_epoch SOURCES test_voice_epoch.c LIBS zone_hub unity STRICT)
# A binding change against arming (ZSRC-012, review RF32): the alarm task's reservation that an
# arm cancels before the claim, and the hold from the claim until the change is made, with the
# real engine, hub and 433 MHz source, arming tried at every step of the change.
alarm_add_test(zone_bind_gate SOURCES test_bind_gate.c LIBS zone_hub alarm_sim unity STRICT)
