# rf_decode - the pulse-train decoders for one-way RF sensors (docs/features/25): what a 433 MHz
# door contact, PIR or remote sends, taken apart into a code word, on the panel (fed by the
# CC1101 + RMT capture in components/drivers/rf_rx) and on the PC (the host tests feed it
# recorded and encoded pulses; the PC simulator hands packets straight to the 433 MHz source).
# Portable C11, no heap, no RTOS, no radio.
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests (see
# host/CMakeLists.txt).
set(RF_DECODE_SRCS
    src/rf_decode.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${RF_DECODE_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(rf_decode STATIC ${RF_DECODE_SRCS})
    target_include_directories(rf_decode PUBLIC include)
    target_compile_features(rf_decode PUBLIC c_std_11)
    set_target_properties(rf_decode PROPERTIES C_EXTENSIONS OFF)
endif()
