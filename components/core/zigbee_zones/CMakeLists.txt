# zigbee_zones - the panel's Zigbee devices (docs/features/20 devices.md, alarm-devices.md;
# BL-143): the device registry and install codes, each device's setup, the operations the Zigbee
# pages ask for (ui_backend's zb_* contract), the events and what is stored (zbz_panel), and the
# alarm devices - IAS WD sirens (zbz_wd) and IAS ACE keypads and key fobs (zbz_ace). The zones'
# side stays zone_hub's zsrc_zigbee. Portable C11; zigbee_service is its one user in the
# firmware.
#
# Built milestone by milestone (docs/features/20 "Milestones"): ZB-M2 the ZCL codec and the
# device registry, ZB-M3 the setup and operations (zbz_panel, zbz_models), ZB-M4 the install
# codes, ZB-M10 the sirens, ZB-M11 the keypads and fobs.
set(ZIGBEE_ZONES_SRCS
    src/zbz_ace.c
    src/zbz_codes.c
    src/zbz_models.c
    src/zbz_panel.c
    src/zbz_registry.c
    src/zbz_wd.c
    src/zbz_zcl.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ZIGBEE_ZONES_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES zone_hub ui_backend alarm_core)
else()
    add_library(zigbee_zones STATIC ${ZIGBEE_ZONES_SRCS})
    target_include_directories(zigbee_zones PUBLIC include)
    target_link_libraries(zigbee_zones PUBLIC zone_hub ui_backend alarm_core)
    target_compile_features(zigbee_zones PUBLIC c_std_11)
    set_target_properties(zigbee_zones PROPERTIES C_EXTENSIONS OFF)

    # The fake radio and its virtual devices built from the device signatures (docs/features/20
    # testing.md §2; ZB-M3): the host tests, and later host/ui_preview and host/ui_sim, link it.
    # Never in the firmware. Held to the portable components' own warnings.
    add_library(zigbee_zones_fake STATIC EXCLUDE_FROM_ALL fake/zbz_fake_radio.c)
    target_include_directories(zigbee_zones_fake PUBLIC fake PRIVATE test/signatures)
    target_link_libraries(zigbee_zones_fake PUBLIC zigbee_zones)
    set_target_properties(zigbee_zones_fake PROPERTIES C_EXTENSIONS OFF)
    if(ALARM_GNU_LIKE)
        target_compile_options(zigbee_zones_fake PRIVATE ${ALARM_WARNINGS})
        if(ALARM_SANITIZE)
            target_compile_options(zigbee_zones_fake PRIVATE ${ALARM_SANITIZE_FLAGS})
        endif()
    endif()
endif()
