# home_control - the Devices registry (docs/features/24): the device source interface, the registry
# that merges the sources, the security classes (a user code before an unlock or an opening) and
# the pending action with its result, plus Home Assistant and the panel's Z-Wave outputs as device sources (C11, no heap, no
# RTOS; it never touches the alarm engine).
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests and the
# PC preview (see host/CMakeLists.txt).
set(HOME_CONTROL_SRCS
    src/home_control.c
    src/dsrc_ha.c
    src/dsrc_zw.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${HOME_CONTROL_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(home_control STATIC ${HOME_CONTROL_SRCS})
    target_include_directories(home_control PUBLIC include)
    target_compile_features(home_control PUBLIC c_std_11)
    set_target_properties(home_control PROPERTIES C_EXTENSIONS OFF)
endif()
