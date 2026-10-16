# panel_link - the panel link between a master panel and its keypads (docs/features/11): the
# crypto helpers on a three-primitive port, the v1 frame and replay window, the message codecs,
# pairing and the session handshake, the master's and the keypad's link managers, the stored
# records, the keypads' status and configuration copy, what a keypad's voice hears
# (plink_vfeed), and Smart Home's device list and records built from home_control's registry
# (plink_home). Portable C11, no heap, no globals; the sockets, NVS and PSA are the platform's
# (components/services/plink_service, host/link_sim).
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests.
set(PANEL_LINK_SRCS
    src/plink_crypto.c
    src/plink_frame.c
    src/plink_wire.c
    src/plink_msg.c
    src/plink_view.c
    src/plink_store.c
    src/plink_pair.c
    src/plink_session.c
    src/plink_home.c
    src/plink_master.c
    src/plink_keypad.c
    src/plink_vfeed.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${PANEL_LINK_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES alarm_core home_control
                           PRIV_REQUIRES alarm_proto zwave_host) # SHA-256; AES-CCM and X25519
else()
    add_library(panel_link STATIC ${PANEL_LINK_SRCS})
    target_include_directories(panel_link PUBLIC include)
    target_link_libraries(panel_link PUBLIC alarm_core home_control
                          PRIVATE alarm_proto zwave_host)
    target_compile_features(panel_link PUBLIC c_std_11)
    set_target_properties(panel_link PROPERTIES C_EXTENSIONS OFF)
endif()
