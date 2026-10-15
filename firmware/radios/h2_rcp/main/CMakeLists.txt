# WHOLE_ARCHIVE: nothing calls rcp_pending.c by name, so the linker would leave it out.
idf_component_register(SRCS "rcp_main.c" "rcp_pending.c"
                       INCLUDE_DIRS "."
                       REQUIRES openthread ieee802154 esp_event nvs_flash vfs esp_driver_uart
                       WHOLE_ARCHIVE)
# The radio's pending table (docs/bugs/B021, rcp_pending.c): OpenThread's source-match calls and
# the driver's transmit go through rcp_pending.c first.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=otPlatRadioAddSrcMatchShortEntry"
    "-Wl,--wrap=otPlatRadioAddSrcMatchExtEntry"
    "-Wl,--wrap=otPlatRadioClearSrcMatchShortEntry"
    "-Wl,--wrap=otPlatRadioClearSrcMatchExtEntry"
    "-Wl,--wrap=otPlatRadioClearSrcMatchShortEntries"
    "-Wl,--wrap=otPlatRadioClearSrcMatchExtEntries"
    "-Wl,--wrap=esp_ieee802154_transmit")
