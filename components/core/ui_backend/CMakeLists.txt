# ui_backend - the contract between the touch UI and whatever runs it (ADR-012; docs/features/33
# step 5). Header only and portable: the UI, the ESP port, the Panel Manager and the PC builds
# all use it.
if(ESP_PLATFORM)
    idf_component_register(INCLUDE_DIRS include
                           REQUIRES alarm_core automation event_log home_control notify zone_hub
                                    storage i18n voice_pack)
else()
    add_library(ui_backend INTERFACE)
    target_include_directories(ui_backend INTERFACE include)
    target_link_libraries(ui_backend INTERFACE alarm_core automation event_log home_control notify
                                               zone_hub storage i18n voice_pack)
endif()
