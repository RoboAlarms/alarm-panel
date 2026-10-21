# zone_hub - zone sources (docs/features/22): the source interface, the hub that binds sensors to
# zones (mapping, grace timers, the binding blob), the input debouncer, and the portable
# simulator, wired, Home Assistant, Zigbee, 433 MHz and Z-Wave sources (C11, no heap, no RTOS;
# pins come through a GPIO port, Home Assistant entities through ha_link, Zigbee devices through
# zigbee_host, Z-Wave devices through zwave_zones).
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests and the
# PC preview (see host/CMakeLists.txt).
set(ZONE_HUB_SRCS
    src/hub.c
    src/bind_codec.c
    src/policy.c
    src/debounce.c
    src/eol.c
    src/zsrc_sim.c
    src/zsrc_wired.c
    src/zsrc_ha.c
    src/zsrc_zigbee.c
    src/zb_quirks.c
    src/zsrc_rf.c
    src/zsrc_zwave.c
    src/zw_quirks.c
    src/voice_epoch.c
    src/bind_gate.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ZONE_HUB_SRCS}
                           INCLUDE_DIRS include
                           PRIV_INCLUDE_DIRS src
                           REQUIRES alarm_core i18n)
else()
    add_library(zone_hub STATIC ${ZONE_HUB_SRCS})
    target_include_directories(zone_hub PUBLIC include PRIVATE src)
    target_link_libraries(zone_hub PUBLIC alarm_core i18n)
    target_compile_features(zone_hub PUBLIC c_std_11)
    set_target_properties(zone_hub PROPERTIES C_EXTENSIONS OFF)
endif()
