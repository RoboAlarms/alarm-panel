# Host tests of the alarm engine (Unity), added by host/CMakeLists.txt; not part of the
# ESP-IDF build. Tests carry the CORE-XXX-NNN requirement IDs of docs/features/01.

# The engine harness: a configured engine, a clock and helpers to drive it. The zone_hub
# tests use it too.
add_library(alarm_sim STATIC sim.c)
target_include_directories(alarm_sim PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(alarm_sim PUBLIC alarm_core unity)
if(ALARM_GNU_LIKE)
    target_compile_options(alarm_sim PRIVATE ${ALARM_TEST_WARNINGS})
endif()

foreach(suite arming exit_entry alarms life_safety users troubles keyseq persistence
              partitions config program codec walk snapshot automation)
    alarm_add_test(${suite} SOURCES test_${suite}.c LIBS alarm_sim)
endforeach()
