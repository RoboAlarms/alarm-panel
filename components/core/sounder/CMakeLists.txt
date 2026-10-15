# sounder - the keypad sounder's patterns and mixer (docs/features/08).
# The single copy of what the panel sounds like: the panel's I2S speakers, the PC simulator's
# sound card and the host tests all render the same blocks from it, so what you hear at the
# desk is what the panel plays on the wall. Portable C11, no heap, no RTOS, no audio device.
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests and
# the simulator (see host/CMakeLists.txt and host/ui_sim/CMakeLists.txt).
set(SOUNDER_SRCS
    src/sounder_mix.c
    src/speech_boost.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${SOUNDER_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES alarm_core)
else()
    add_library(sounder STATIC ${SOUNDER_SRCS})
    target_include_directories(sounder PUBLIC include)
    target_link_libraries(sounder PUBLIC alarm_core)
    target_compile_features(sounder PUBLIC c_std_11)
    set_target_properties(sounder PROPERTIES C_EXTENSIONS OFF)
    if(UNIX)
        target_link_libraries(sounder PUBLIC m) # sinf/expf/log10f; MinGW and newlib carry them already
    endif()
endif()
