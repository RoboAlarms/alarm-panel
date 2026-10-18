# The Panel Manager's portable C on the PC. The page tests (*.test.cjs) run under Node, not here.
# cJSON is fetched at configure time at the release the firmware's component manager takes
# (espressif/cjson ^1.7.19, ../idf_component.yml): upstream's cJSON.c alone, built here.
include(FetchContent)
FetchContent_Declare(alarm_cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG c859b25da02955fef659d658b8f324b5cde87be3  # v1.7.19
    SOURCE_SUBDIR no-cmake-build)  # sources only: upstream's CMakeLists is not added
FetchContent_MakeAvailable(alarm_cjson)
if(NOT TARGET web_cjson)
    add_library(web_cjson STATIC ${alarm_cjson_SOURCE_DIR}/cJSON.c)
    # Someone else's code: its headers raise no warnings in our strict targets.
    target_include_directories(web_cjson SYSTEM PUBLIC ${alarm_cjson_SOURCE_DIR})
endif()

# Review 3 #6: the config/display merge loops under a cJSON allocator that fails.
alarm_add_test(web_json_merge
               SOURCES test_web_json_merge.c ${CMAKE_CURRENT_LIST_DIR}/../src/web_json_merge.c
               LIBS web_cjson unity INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../src STRICT)

# Code review S01: web_login.c itself, against the fakes in test_web_login.c. The ESP-IDF
# headers it includes are written here as empty files; the test declares what the file uses.
set(web_login_fakes ${CMAKE_CURRENT_BINARY_DIR}/web_login_fakes)
foreach(header alarm_mem.h alarm_service.h code_hash.h esp_attr.h esp_err.h esp_log.h esp_mac.h
               esp_random.h esp_timer.h freertos/FreeRTOS.h freertos/semphr.h
               mbedtls/platform_util.h nvs.h psa/crypto.h)
    file(WRITE ${web_login_fakes}/${header} "/* empty: see test_web_login.c */\n")
endforeach()
alarm_add_test(web_login
               SOURCES test_web_login.c
               LIBS i18n alarm_core unity
               INCLUDES ${web_login_fakes} ${CMAKE_CURRENT_LIST_DIR}/../src
                        ${CMAKE_CURRENT_LIST_DIR}/../include)

# Code review M01 (attaching JSON children), S06 (WEB-016: whose Activity shows which
# events) and PERF11 (a WebSocket message's time limit).
alarm_add_test(web_rules
               SOURCES test_web_rules.c ${CMAKE_CURRENT_LIST_DIR}/../src/web_rules.c
               LIBS web_cjson event_log alarm_core unity INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../src STRICT)

# The real keypad HTTP service compiled with and without a local display.
alarm_add_test(web_setup_flow
    SOURCES test_web_setup_flow.c ${CMAKE_CURRENT_LIST_DIR}/../src/web_setup_flow.c
    LIBS unity INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../src STRICT)

set(keypad_fakes ${CMAKE_CURRENT_BINARY_DIR}/keypad_fakes)
foreach(header alarm_mem.h alarm_service.h panel_prefs.h esp_timer.h sdkconfig.h plink_service.h nvs.h)
    file(WRITE ${keypad_fakes}/${header} "/* declared by test_web_keypads.c */\n")
endforeach()
foreach(mode normal headless)
    alarm_add_test(web_keypads_${mode} SOURCES test_web_keypads.c LIBS ui_backend alarm_core web_cjson unity
        INCLUDES ${keypad_fakes} ${CMAKE_CURRENT_LIST_DIR}/../src STRICT)
endforeach()
target_compile_definitions(test_web_keypads_normal PRIVATE CONFIG_ALARM_FEATURE_UI=1)
target_compile_definitions(test_web_keypads_headless PRIVATE CONFIG_ALARM_FEATURE_UI=0)
