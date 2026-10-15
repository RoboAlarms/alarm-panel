# Automations (docs/features/41): one suite per test_<suite>.c here, tagged AUT-NNN, each linked
# with the engine simulator (alarm_sim: alarm_core and unity) for the observer's and runner's
# end-to-end cases.
file(GLOB AUTOMATION_TESTS CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} test_*.c)
foreach(test_file ${AUTOMATION_TESTS})
    string(REGEX REPLACE "^test_(.*)\\.c$" "\\1" suite ${test_file})
    alarm_add_test(${suite} SOURCES ${test_file} LIBS automation alarm_sim)
endforeach()
