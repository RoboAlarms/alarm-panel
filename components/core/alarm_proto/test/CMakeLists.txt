# MQTT messages and store-and-forward (MQTT-NNN, docs/features/10): golden JSON, command parsing,
# Home Assistant discovery, replay from the event log; firmware-update helpers (OTA-NNN,
# docs/features/13); the local Home Assistant link (framing, pairing, hello; docs/features/23).
foreach(suite proto report link pair zwdev)
    alarm_add_test(${suite} SOURCES test_${suite}.c LIBS alarm_proto notify unity)
endforeach()
# test_fw also checks the update task's retry schedule and signed-profile rule
# (alarm_proto/ota_retry.h, alarm_proto/ota_profile.h).
alarm_add_test(fw SOURCES test_fw.c LIBS alarm_proto unity)
# test_fwsrv: the firmware server - domain, check, languages, signing keys, trust record (OTA-017..OTA-025).
alarm_add_test(fwsrv SOURCES test_fwsrv.c LIBS alarm_proto unity)
# test_prefs: the settings fields on the panel link and in the Home Assistant link's JSON
# (docs/features/11 LINK-050..052, docs/features/23 HAI-017).
alarm_add_test(prefs SOURCES test_prefs.c LIBS alarm_proto unity)
