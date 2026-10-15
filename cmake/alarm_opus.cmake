# The Opus decoder for every PC build (docs/features/08 voice-first-build.md D10): the host tests,
# and later the simulator (host/ui_sim) and the preview (host/ui_preview), each with one line:
#   include(${ALARM_REPO}/cmake/alarm_opus.cmake)
# It fetches esphome-libs/micro-opus at the exact commit the firmware's component manager pins
# (esphome/micro-opus 0.4.1, components/services/panel_voice/idf_component.yml) with its libopus
# submodule, builds upstream's own micro_opus target (fixed point, OPUS_ALLOCATION_MODE=USE_ALLOCA,
# like the panel) and the voice_codec adapter over it. Never select another libopus: the commit's
# gitlink is the version.
#
# Targets: micro_opus (upstream; its warnings are its own, never -Werror), voice_codec (our
# adapter) and voice_render (the utterance renderer), held to the portable components' warnings.
include_guard(GLOBAL)

include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/alarm_host.cmake)

# micro-opus's host build compiles its Ogg reader (C++) along with libopus.
enable_language(CXX)

set(ALARM_OPUS_GIT_TAG 8354085908683c6130e32a832aeec8a7ca115c51)  # micro-opus 0.4.1
set(OPUS_ALLOCATION_MODE "USE_ALLOCA" CACHE STRING "micro-opus scratch allocation (D10)" FORCE)

FetchContent_Declare(micro_opus
    GIT_REPOSITORY https://github.com/esphome-libs/micro-opus.git
    GIT_TAG ${ALARM_OPUS_GIT_TAG}
    GIT_SUBMODULES_RECURSE ON
    GIT_SHALLOW OFF)
FetchContent_MakeAvailable(micro_opus)

# Its headers are someone else's: no warnings from them in our strict targets.
get_target_property(_alarm_opus_inc micro_opus INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(micro_opus PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_alarm_opus_inc}")
# Upstream is built as upstream builds it: GNU C (its alloca comes from <stdlib.h> there, which
# our strict C11 hides) and its own warnings, never errors (our -Werror is set per target).
foreach(_alarm_opus_lib micro_opus micro_ogg_demuxer)
    if(TARGET ${_alarm_opus_lib})
        set_target_properties(${_alarm_opus_lib} PROPERTIES C_EXTENSIONS ON CXX_EXTENSIONS ON)
    endif()
endforeach()

if(NOT TARGET voice_codec)
    add_library(voice_codec STATIC ${ALARM_COMPONENTS_DIR}/services/panel_voice/src/voice_codec.c)
    target_include_directories(voice_codec PUBLIC ${ALARM_COMPONENTS_DIR}/services/panel_voice/include)
    target_link_libraries(voice_codec PUBLIC micro_opus)
    if(ALARM_GNU_LIKE)
        target_compile_options(voice_codec PRIVATE ${ALARM_WARNINGS})
    endif()
endif()

# The utterance renderer over the codec and the pack reader (D11): plain C11, the same file the
# panel compiles.
if(NOT TARGET voice_render)
    add_library(voice_render STATIC ${ALARM_COMPONENTS_DIR}/services/panel_voice/src/voice_render.c)
    target_include_directories(voice_render PUBLIC ${ALARM_COMPONENTS_DIR}/services/panel_voice/include)
    target_link_libraries(voice_render PUBLIC voice_codec voice_pack)
    if(ALARM_GNU_LIKE)
        target_compile_options(voice_render PRIVATE ${ALARM_WARNINGS})
    endif()
endif()
