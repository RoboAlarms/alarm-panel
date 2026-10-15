# annunciator - what the panel says, and when (docs/features/08 voice-first-build.md D04, D05,
# D06, D11; docs/features/40 WALK-016): the voice's request policy. It turns the engine's events,
# one-shot tones and snapshots into utterances (clip IDs and the silence between them) and decides
# which one plays - one active, four waiting, by priority, with coalescing, expiry and
# cancellation. Portable C11, no heap, no RTOS, no audio: the platform's voice worker decodes.
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests.
set(ANNUNCIATOR_SRCS
    src/annunciator.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ANNUNCIATOR_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES alarm_core voice_pack)
else()
    add_library(annunciator STATIC ${ANNUNCIATOR_SRCS})
    target_include_directories(annunciator PUBLIC include)
    target_link_libraries(annunciator PUBLIC alarm_core voice_pack)
    target_compile_features(annunciator PUBLIC c_std_11)
    set_target_properties(annunciator PROPERTIES C_EXTENSIONS OFF)
endif()
