idf_component_register(SRCS "src/panel_web.c" "src/web_keypads.c" "src/web_config.c" "src/web_extra.c" "src/web_login.c" "src/web_screen.c" "src/web_zwave.c" "src/web_zigbee.c" "src/web_automations.c" "src/web_voice.c" "src/web_json_merge.c" "src/web_rules.c" "src/web_setup.c" "src/web_setup_flow.c" "src/web_setup_dns.c" "src/web_setup_usb.c"
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
    REQUIRES ui_backend dev_remote
    PRIV_REQUIRES plink_service alarm_mem backup_service lang_packs panel_voice voice_pack ui_port_esp platform_esp panel_ota panel_storage alarm_core net_mqtt net_notify notify zwave_service zigbee_service automation_service device_service i18n espressif__cjson esp_https_server esp_http_server http_redirect
        esp_timer esp_hw_support app_update mbedtls nvs_flash console esp_driver_usb_serial_jtag)
# The browser interface, in web/ (web/README.md); the mock-*.cjs files there are PC only and
# are not embedded. Each file goes in gzipped (a quarter of the flash, and a quarter of the
# bytes over TLS); panel_web.c sends it as it is to browsers and inflates it for a client that
# doesn't take gzip.
idf_build_get_property(python PYTHON)
foreach(asset panel.html panel.css panel.js panel-pages.js panel-keypad.js panel-words.js)
    set(src ${CMAKE_CURRENT_LIST_DIR}/web/${asset})
    set(gz ${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz)
    add_custom_command(OUTPUT ${gz}
        COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py ${src} ${gz}
        DEPENDS ${src} ${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py ${CMAKE_CURRENT_LIST_DIR}/minify_asset.py
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} ${gz} BINARY)
endforeach()
# Public JSON parsing must stay bounded on the HTTPS worker's stack.
if(CONFIG_CJSON_NESTING_LIMIT GREATER 12)
    message(FATAL_ERROR "Panel Manager requires CONFIG_CJSON_NESTING_LIMIT <= 12")
endif()
