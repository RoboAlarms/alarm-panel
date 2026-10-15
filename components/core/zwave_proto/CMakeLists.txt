# zwave_proto - the Z-Wave Serial API frame layer between the panel and the ZAC93
# (docs/features/36, serial-api.md §1 "The wire"), a CRC-16 helper, DSK/PIN text helpers, the
# Serial API's own commands (§2-§6) and the application-layer command classes
# (command-classes.md). Portable C11; components/core/zwave_host runs the send/retry transport,
# the start-up sequence and the security layer on top of it.
set(ZWAVE_PROTO_SRCS
    src/zw_frame.c
    src/zw_crc16.c
    src/zw_dsk.c
    src/zw_api_lists.c
    src/zw_api_setup.c
    src/zw_api_startup.c
    src/zw_api_network.c
    src/zw_api_transport.c
    src/zw_cc_internal.c
    src/zw_cc_basic.c
    src/zw_cc_notification.c
    src/zw_cc_supervision.c
    src/zw_cc_multichannel.c
    src/zw_cc_network.c
    src/zw_cc_misc.c
    src/zw_cc_entry_control.c
    src/zw_cc_encap.c
    src/zw_cc_match.c
    src/zw_cc_answer.c
    src/zw_redact.c
    src/zw_cc_fwu.c
    src/zw_fwfile.c
    src/zw_gbl.c
    src/zw_cc_switch.c
    src/zw_sha1.c
    src/zw_qr.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ZWAVE_PROTO_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(zwave_proto STATIC ${ZWAVE_PROTO_SRCS})
    target_include_directories(zwave_proto PUBLIC include)
    target_compile_features(zwave_proto PUBLIC c_std_11)
    set_target_properties(zwave_proto PROPERTIES C_EXTENSIONS OFF)
endif()
