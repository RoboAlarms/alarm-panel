# event_log - power-fail-safe event log ring in raw flash (C11, no heap, no RTOS).
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests and
# the PC preview (see host/CMakeLists.txt).
set(EVENT_LOG_SRCS src/elog.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${EVENT_LOG_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES alarm_core)
else()
    add_library(event_log STATIC ${EVENT_LOG_SRCS})
    target_include_directories(event_log PUBLIC include)
    target_link_libraries(event_log PUBLIC alarm_core)
    target_compile_features(event_log PUBLIC c_std_11)
    set_target_properties(event_log PROPERTIES C_EXTENSIONS OFF)
endif()
