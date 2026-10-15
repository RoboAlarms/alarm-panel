# zigbee_service - the ESP32-H2 sensor radio as a zone source (docs/features/20): bring-up over the
# socket's UART (radio_port) and the Zigbee source's supervision; since ZB-M6 (BL-143) it owns
# zigbee_zones' zbz_panel (the devices, their setup, sirens and keypads), the console's `zigbee`
# and the developer link's /dev/v1/zigbee. Optional: ALARM_ZSRC_ZIGBEE; off, it answers "not in
# this firmware" (stubs).
idf_component_register(SRCS "src/zigbee_service.c" "src/radio_port.c" "src/zigbee_console.c"
                            "src/zigbee_dev.c" "src/zigbee_bench.c"
                       INCLUDE_DIRS "include"
                       REQUIRES zigbee_host zone_hub zigbee_zones alarm_core ui_backend
                       PRIV_REQUIRES platform_esp alarm_mem alarm_proto console
                                     esp_driver_uart esp_timer nvs_flash)

# esp-zigbee-lib's esp32p4 build traps on P4 v1.x silicon in esp_zigbee_task_queue_post()
# (docs/bugs/B024): radio_port.c stands in for it on those builds.
if(CONFIG_ALARM_ZSRC_ZIGBEE AND CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_zigbee_task_queue_post")
endif()
# Restarting the stack in one boot (B024): ESP-IDF's radio Spinel layer never resets its
# SpinelDriver, so radio_port.c resets it before Init; and a stop during a transmission makes the
# radio refuse to sleep, which esp-zigbee aborts on, so radio_port.c takes it back to receiving.
# The radio's pending table (B021): radio_port.c learns where the RadioSpinel is from the add calls
# and clears single addresses, which esp-zigbee-lib 2.0.4 left as stubs.
if(CONFIG_ALARM_ZSRC_ZIGBEE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=_ZN2ot6Spinel12SpinelDriver4InitERNS0_15SpinelInterfaceEbPKhh"
        "-Wl,--wrap=_ZN2ot6Spinel12SpinelDriver34SetCoprocessorResetFailureCallbackEPFvPvES2_"
        "-Wl,--wrap=esp_radio_spinel_sleep"
        "-Wl,--wrap=_ZN2ot6Spinel11RadioSpinel21AddSrcMatchShortEntryEt"
        "-Wl,--wrap=_ZN2ot6Spinel11RadioSpinel19AddSrcMatchExtEntryERK12otExtAddress")
endif()
