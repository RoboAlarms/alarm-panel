# h264_sps - the remote keypad's H.264 key frames rewritten to show at once in a browser: no
# reordering, one picture buffered, BT.601 colour (docs/features/31). Portable C11, no heap: the
# host tests and the P4's remote keypad use it.
set(H264_SPS_SRCS
    src/h264_sps.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${H264_SPS_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(h264_sps STATIC ${H264_SPS_SRCS})
    target_include_directories(h264_sps PUBLIC include)
    target_compile_features(h264_sps PUBLIC c_std_11)
    set_target_properties(h264_sps PROPERTIES C_EXTENSIONS OFF)
endif()
