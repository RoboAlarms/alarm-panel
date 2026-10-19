# device_service - the Devices registry (home_control) and Home Assistant as a device source
# (docs/features/24), and the panel's own Z-Wave outputs as one (docs/features/41 phase B).
# Optional: ALARM_HOME_CONTROL (needs PSRAM and ALARM_FEATURE_HA or ALARM_ZSRC_ZWAVE); off, the
# panel has no Devices screen and the service answers with stubs (HCTL-010).
idf_component_register(SRCS "src/device_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES alarm_core home_control
                       PRIV_REQUIRES alarm_mem alarm_proto platform_esp rtc_clock zone_hub console
                                     zwave_service)
