# board_info - a board described in data, and the check that the description is safe
# (docs/features/33 step 4, ADR-035). Portable C11, no heap: the host tests and every board use it.
set(BOARD_INFO_SRCS
    src/board_info.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${BOARD_INFO_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(board_info STATIC ${BOARD_INFO_SRCS})
    target_include_directories(board_info PUBLIC include)
    target_compile_features(board_info PUBLIC c_std_11)
    set_target_properties(board_info PROPERTIES C_EXTENSIONS OFF)
endif()
