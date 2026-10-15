# The voice's Opus adapter on the PC (AUD-028): a tone encoded here with libopus's own encoder,
# decoded through voice_codec with D08's pre-skip and final trim. voice_codec and micro_opus come
# from cmake/alarm_opus.cmake (host/CMakeLists.txt includes it).
alarm_add_test(voice_codec SOURCES test_voice_codec.c LIBS voice_codec unity STRICT)

# The utterance renderer (D11): the upsampler on worked vectors, and the renderer on the tracked
# en-US voice, packed at build time by tools/voice/voicepack.py (standard library only). Without a
# Python 3 the pack tests are ignored, not failed.
alarm_add_test(voice_render SOURCES test_voice_render.c LIBS voice_render unity STRICT)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(_voice_pack ${CMAKE_CURRENT_BINARY_DIR}/test_voice.rvoc)
    file(GLOB_RECURSE _voice_inputs ${ALARM_REPO}/assets/voice/en-US/*.opuspkt
         ${ALARM_REPO}/assets/voice/en-US/*/voice.json)
    add_custom_command(OUTPUT ${_voice_pack}
        COMMAND ${Python3_EXECUTABLE} ${ALARM_REPO}/tools/voice/voice.py pack --unsigned
                --allow-draft --out ${_voice_pack}
        DEPENDS ${_voice_inputs} ${ALARM_REPO}/tools/voice/voicepack.py
        WORKING_DIRECTORY ${ALARM_REPO}
        COMMENT "Packing the tracked en-US voice for test_voice_render"
        VERBATIM)
    add_custom_target(voice_test_pack DEPENDS ${_voice_pack})
    add_dependencies(test_voice_render voice_test_pack)
    target_compile_definitions(test_voice_render PRIVATE VOICE_TEST_PACK="${_voice_pack}")
endif()
