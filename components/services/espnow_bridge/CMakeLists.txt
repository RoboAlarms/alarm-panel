# Dependencies are declared during IDF's early pass, before sdkconfig is available.
idf_component_register(SRCS "src/espnow_bridge.c" INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../core/alarm_proto/include"
    PRIV_REQUIRES esp_wifi esp_event esp_app_format)
if(CONFIG_ESP_HOSTED_CP)
    # Header-only wire: no alarm engine or JSON on the radio.
    target_link_libraries(${COMPONENT_LIB} PRIVATE eh_cp_feat_peer_data)
endif()
