# alarm_core - portable alarm engine (C11, no heap, no RTOS).
# Builds as an ESP-IDF component, or as a plain CMake static library for the
# host tests / PC simulator (see host/CMakeLists.txt).
set(ALARM_CORE_SRCS
    src/alarms.c
    src/arming.c
    src/codec.c
    src/config.c
    src/engine.c
    src/events.c
    src/keyseq.c
    src/program.c
    src/snapshot.c
    src/strings.c
    src/users.c
    src/walk.c
    src/zones.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ALARM_CORE_SRCS}
                           INCLUDE_DIRS include
                           PRIV_INCLUDE_DIRS src)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        ALARM_MAX_PARTITIONS=${CONFIG_ALARM_MAX_PARTITIONS}
        ALARM_MAX_ZONES=${CONFIG_ALARM_MAX_ZONES}
        ALARM_MAX_USERS=${CONFIG_ALARM_MAX_USERS}
        ALARM_MAX_DEVICES=${CONFIG_ALARM_MAX_DEVICES})
else()
    add_library(alarm_core STATIC ${ALARM_CORE_SRCS})
    target_include_directories(alarm_core PUBLIC include PRIVATE src)
    target_compile_features(alarm_core PUBLIC c_std_11)
    set_target_properties(alarm_core PROPERTIES C_EXTENSIONS OFF)
endif()
