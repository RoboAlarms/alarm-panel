# alarm_proto - the panel's MQTT messages: JSON writer/reader, status/zone/event payloads,
# commands, Home Assistant discovery, the store-and-forward reporter and firmware-update helpers
# (C11, no heap).
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests.
set(ALARM_PROTO_SRCS
    src/json_write.c
    src/json_read.c
    src/messages.c
    src/commands.c
    src/discovery.c
    src/report.c
    src/fw.c
    src/fwsrv.c
    src/sha256.c
    src/link.c
    src/pair.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ALARM_PROTO_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES alarm_core event_log
                           PRIV_REQUIRES i18n notify) # its words for the screen are marked (I18N_NOOP_C)
else()
    add_library(alarm_proto STATIC ${ALARM_PROTO_SRCS})
    target_include_directories(alarm_proto PUBLIC include)
    target_link_libraries(alarm_proto PUBLIC alarm_core event_log PRIVATE i18n notify)
    target_compile_features(alarm_proto PUBLIC c_std_11)
    set_target_properties(alarm_proto PROPERTIES C_EXTENSIONS OFF)
endif()
