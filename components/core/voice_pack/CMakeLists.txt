# voice_pack - the reader of an RVOC format 1 voice pack (docs/features/08 voice-first-build.md
# D08): validates a pack the platform has already checked the signature of, and gives its clips,
# packets and picker words to the annunciator's policy, the voice service and the tools.
# Portable C11, no heap, no RTOS, no audio decoding. SHA-256 comes from alarm_proto (proto_sha256).
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests.
set(VOICE_PACK_SRCS
    src/voice_pack.c
    src/vp_words.c
    src/vp_accept.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${VOICE_PACK_SRCS}
                           INCLUDE_DIRS include
                           PRIV_REQUIRES alarm_proto)
else()
    add_library(voice_pack STATIC ${VOICE_PACK_SRCS})
    target_include_directories(voice_pack PUBLIC include)
    target_link_libraries(voice_pack PRIVATE alarm_proto)
    target_compile_features(voice_pack PUBLIC c_std_11)
    set_target_properties(voice_pack PROPERTIES C_EXTENSIONS OFF)
endif()
