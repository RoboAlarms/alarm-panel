# sirens - every kind of siren the panel drives, one policy (docs/features/16): the engine's bell
# and strobe outputs per partition, what each siren sounds for (siren.c), the wired outputs with
# their temporal patterns, squawks and bell supervision (out_wired.c), and the 433 MHz sirens the
# panel transmits to (rf_siren.c). Portable C11, no heap, no RTOS.
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests.
set(SIRENS_SRCS
    src/siren.c
    src/out_wired.c
    src/rf_siren.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${SIRENS_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES alarm_core rf_decode)
else()
    add_library(sirens STATIC ${SIRENS_SRCS})
    target_include_directories(sirens PUBLIC include)
    target_link_libraries(sirens PUBLIC alarm_core rf_decode)
    target_compile_features(sirens PUBLIC c_std_11)
    set_target_properties(sirens PROPERTIES C_EXTENSIONS OFF)
endif()
