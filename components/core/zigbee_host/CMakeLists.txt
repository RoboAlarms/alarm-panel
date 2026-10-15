# zigbee_host - the panel's side of the sensor radio (docs/features/20). Portable C11, no heap, no
# RTOS: radio bring-up policy tested with a fake transport. The platform transport, image,
# coordinator and updater remain unimplemented; see docs/features/20/README.md.
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests.
set(ZIGBEE_HOST_SRCS
    src/bringup.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ZIGBEE_HOST_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(zigbee_host STATIC ${ZIGBEE_HOST_SRCS})
    target_include_directories(zigbee_host PUBLIC include)
    target_compile_features(zigbee_host PUBLIC c_std_11)
    set_target_properties(zigbee_host PROPERTIES C_EXTENSIONS OFF)
endif()
