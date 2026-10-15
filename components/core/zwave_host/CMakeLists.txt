# zwave_host - the panel's Z-Wave host (docs/features/36): the S2/S0 crypto layer over a port of
# three primitives (security.md §2-3, §6, §10; ZW-008), the Serial API transport with its
# request scheduler (serial-api.md §1, §2, §6.8, §6.12, §7; ZW-003) and the host that starts and
# keeps the link (§4, §5, §7; ZW-001, ZW-004), the S2 security manager (security.md §6-9; ZW-007,
# ZW-009), adding devices (network.md §4, security.md §5; ZW-005, ZW-006), the node cache (§2;
# ZW-017), the interview (§5-6; ZW-010), wake-ups and supervision (§7-8; ZW-011, ZW-012) and
# removing devices (§10; ZW-016), wired together in zw_net with the checks at every start (§2),
# region and reset (§12; ZW-018). Portable C11 on zwave_proto's frames and codecs; the firmware's
# crypto port on PSA comes at Z8, the PC one is test/zw_host_crypto.c.
set(ZWAVE_HOST_SRCS
    src/zw_aes_modes.c
    src/zw_crypto.c
    src/zw_crypto_util.c
    src/zw_ctr_drbg.c
    src/zw_health.c
    src/zw_host.c
    src/zw_host_steps.c
    src/zw_incl.c
    src/zw_iv.c
    src/zw_iv_steps.c
    src/zw_keys.c
    src/zw_net.c
    src/zw_net_nodes.c
    src/zw_nodes.c
    src/zw_region.c
    src/zw_remove.c
    src/zw_fwupd.c
    src/zw_prov.c
    src/zw_net_smart.c
    src/zw_jam.c
    src/zw_routes.c
    src/zw_net_routes.c
    src/zw_net_power.c
    src/zw_s2.c
    src/zw_s2_cc.c
    src/zw_s2_kdf.c
    src/zw_transport.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ZWAVE_HOST_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES zwave_proto)
else()
    add_library(zwave_host STATIC ${ZWAVE_HOST_SRCS})
    target_include_directories(zwave_host PUBLIC include)
    target_link_libraries(zwave_host PUBLIC zwave_proto)
    target_compile_features(zwave_host PUBLIC c_std_11)
    set_target_properties(zwave_host PROPERTIES C_EXTENSIONS OFF)
endif()
