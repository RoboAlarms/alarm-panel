# ui_remote - the touch UI's backend over a byte channel (docs/features/11, LINK-057..LINK-065):
# the calls' ids and their arguments and answers, the alarm panel's side (ui_remote_serve, under a
# keypad's grant) and the keypad's proxies (ui_remote_proxy). Portable C11, no heap, no globals;
# the channel is the panel link's (components/services/plink_service, host/link_sim).
# Builds as an ESP-IDF component, or as a plain CMake static library for the host tests.
set(UI_REMOTE_SRCS
    src/ur_codec.c
    src/ur_serve.c
    src/ur_proxy.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${UI_REMOTE_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES ui_backend
                           PRIV_REQUIRES alarm_core panel_link)
else()
    add_library(ui_remote STATIC ${UI_REMOTE_SRCS})
    target_include_directories(ui_remote PUBLIC include)
    target_link_libraries(ui_remote PUBLIC ui_backend PRIVATE alarm_core panel_link)
    target_compile_features(ui_remote PUBLIC c_std_11)
    set_target_properties(ui_remote PROPERTIES C_EXTENSIONS OFF)
endif()
