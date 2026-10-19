# Only esp_err_t from ESP-IDF is needed by panel_ota.h for these portable status tests.
set(cp_result_fakes ${CMAKE_CURRENT_BINARY_DIR}/cp_result_fakes)
file(WRITE ${cp_result_fakes}/esp_err.h "#pragma once\ntypedef int esp_err_t;\n")
alarm_add_test(cp_ota_result SOURCES test_cp_ota_result.c
    LIBS alarm_core alarm_proto unity
    INCLUDES ${cp_result_fakes} ${CMAKE_CURRENT_LIST_DIR}/../include
             ${CMAKE_CURRENT_LIST_DIR}/../src STRICT)
