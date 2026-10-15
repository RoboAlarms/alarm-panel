# notify - what the panel says about an event, in words (docs/features/26).
# The single copy of the event sentences: the Activity list on the panel and a push notification
# on a phone both read them from here, so the two can never drift apart. Portable C11, no heap,
# no RTOS, no LVGL - the UI maps the accent and icon ids back to its own colours and glyphs.
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests, the PC
# preview and the simulator (see host/CMakeLists.txt).
set(NOTIFY_SRCS
    src/notify_words.c
    src/notify_auto.c
    src/notify_gate.c
    src/notify_http.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${NOTIFY_SRCS}
                           INCLUDE_DIRS include
                           PRIV_INCLUDE_DIRS src
                           REQUIRES alarm_core i18n automation)
else()
    add_library(notify STATIC ${NOTIFY_SRCS})
    target_include_directories(notify PUBLIC include PRIVATE src)
    target_link_libraries(notify PUBLIC alarm_core i18n automation)
    target_compile_features(notify PUBLIC c_std_11)
    set_target_properties(notify PROPERTIES C_EXTENSIONS OFF)
endif()
