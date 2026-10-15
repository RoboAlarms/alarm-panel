# The voice for the PC builds (docs/features/08 voice-first-build.md): the tracked en-US voice,
# packed unsigned at build time by tools/voice/voice.py, as panel_voice's tests pack it
# (components/services/panel_voice/test/CMakeLists.txt). host_panel.c opens it with vp_open.
# Without a Python 3 there is no pack: the voice reads as UI_VOICE_NONE and everything else works.
#
#   include(${ALARM_REPO}/host/ui_preview/voice_pack.cmake)
#   alarm_host_voice_pack(<target>...)
#
# Every target that compiles host_panel.c calls alarm_host_voice_pack: besides the pack's path it
# links the voice policy and renderer host_panel.c wires (annunciator, voice_render with the Opus
# decoder fetched by cmake/alarm_opus.cmake) and the sounder's mixer. Another voice at run time:
# the environment variable ALARM_VOICE_PACK=<file> (an unsigned pack; tools/ui_sim.ps1 and
# tools/ui_preview.ps1 -VoicePack set it).
include(${ALARM_REPO}/cmake/alarm_opus.cmake)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(ALARM_HOST_VOICE_PACK ${CMAKE_BINARY_DIR}/voice/en-US.rvoc)
    file(GLOB_RECURSE _host_voice_inputs ${ALARM_REPO}/assets/voice/en-US/*.opuspkt
         ${ALARM_REPO}/assets/voice/en-US/*/voice.json)
    add_custom_command(OUTPUT ${ALARM_HOST_VOICE_PACK}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/voice
        COMMAND ${Python3_EXECUTABLE} ${ALARM_REPO}/tools/voice/voice.py pack --unsigned
                --allow-draft --out ${ALARM_HOST_VOICE_PACK}
        DEPENDS ${_host_voice_inputs} ${ALARM_REPO}/tools/voice/voicepack.py
                ${ALARM_REPO}/tools/voice/voice.py
        WORKING_DIRECTORY ${ALARM_REPO}
        COMMENT "Packing the tracked en-US voice for the PC builds"
        VERBATIM)
    add_custom_target(host_voice_pack DEPENDS ${ALARM_HOST_VOICE_PACK})
else()
    message(STATUS "No Python 3: the PC builds have no voice (UI_VOICE_NONE)")
endif()

function(alarm_host_voice_pack)
    foreach(t ${ARGN})
        target_link_libraries(${t} PRIVATE annunciator sounder voice_render power_monitor)
        if(TARGET host_voice_pack)
            add_dependencies(${t} host_voice_pack)
            target_compile_definitions(${t} PRIVATE HOST_VOICE_PACK="${ALARM_HOST_VOICE_PACK}")
        endif()
    endforeach()
endfunction()
