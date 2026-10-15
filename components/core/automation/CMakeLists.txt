# automation - the panel's built-in automations (docs/features/41): the model, the validator and the
# stored record (auto_model.c, auto_codec.c), the observer that turns engine events and snapshots
# into occurrences (auto_watch.c), the local clock's crossings (auto_clock.c) and the runner
# (auto_run.c, with arming and disarming in auto_arm.c) and the store over a key-value port (auto_store.c). Portable C11;
# components/services/automation_service runs it on the panel and host/ui_preview/host_panel.c
# in the PC simulator.
set(AUTOMATION_SRCS
    src/auto_model.c
    src/auto_codec.c
    src/auto_watch.c
    src/auto_clock.c
    src/auto_run.c
    src/auto_arm.c
    src/auto_store.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${AUTOMATION_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES alarm_core home_control rtc_clock)
else()
    add_library(automation STATIC ${AUTOMATION_SRCS})
    target_include_directories(automation PUBLIC include)
    target_link_libraries(automation PUBLIC alarm_core home_control rtc_clock)
    target_compile_features(automation PUBLIC c_std_11)
    set_target_properties(automation PROPERTIES C_EXTENSIONS OFF)
endif()
