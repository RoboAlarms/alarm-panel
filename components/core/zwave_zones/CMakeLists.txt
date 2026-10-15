# zwave_zones - Z-Wave devices as zones (docs/features/36 zones.md, milestone Z6): the glue between
# the panel's Z-Wave network (zwave_host's zw_net) and its zone source (zone_hub's zsrc_zwave) -
# device facts from the node cache, commands decoded for the zones, Supervision and the panel's
# answers, devices asked for their state - shared by the PC simulator and the firmware's
# zwave_service (Z8); and the panel's Z-Wave as the service runs it (zwz_panel, Z8): the network
# with the bridge, what the Z-Wave pages show and ask for (ui_backend's zw_* contract), the logged
# events and what is stored. Portable C11; zwave_service is its one user in the firmware.
set(ZWAVE_ZONES_SRCS
    src/zwave_zones.c
    src/zwz_panel.c
    src/zwz_store.c
    src/zw_keypad.c
    src/zw_feedback.c
    src/zw_keyctl.c
    src/zw_fwcat.c
    src/zw_fwjob.c
    src/zw_outputs.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ZWAVE_ZONES_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES zwave_host zwave_proto zone_hub ui_backend alarm_core alarm_proto)
else()
    add_library(zwave_zones STATIC ${ZWAVE_ZONES_SRCS})
    target_include_directories(zwave_zones PUBLIC include)
    target_link_libraries(zwave_zones PUBLIC zwave_host zwave_proto zone_hub ui_backend alarm_core alarm_proto)
    target_compile_features(zwave_zones PUBLIC c_std_11)
    set_target_properties(zwave_zones PROPERTIES C_EXTENSIONS OFF)
endif()
