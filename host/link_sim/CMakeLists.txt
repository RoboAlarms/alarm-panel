# host/link_sim - the panel link on a PC (docs/features/11 LINK-013, docs/features/21 "PC
# simulator"):
#   link_sim    a virtual master (the real engine) or a virtual keypad over UDP, with a console
#               (pwsh tools/link_sim.ps1 master | keypad ...)
#   link_smoke  both in one process over 127.0.0.1 on a virtual clock: a CTest test, run by CI
#               under the sanitizers on Linux
# host/CMakeLists.txt adds this folder with the host tests; it also builds on its own:
#   cmake -S host/link_sim -B build/link_sim -G Ninja && cmake --build build/link_sim
cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(link_sim C)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/alarm_host.cmake)
    alarm_add_core_components(WARNINGS)
    enable_testing()
endif()

set(LINK_CORE ${ALARM_COMPONENTS_DIR}/core)

# The OS side (sockets, clocks, random numbers, the console) and the two nodes. The AES and
# X25519 primitives are zwave_host's PC ones (written from the standards; its test folder).
add_library(link_sim_lib STATIC
    link_udp.c
    link_os_crypto.c
    link_console.c
    link_node.c
    link_fw.c
    ${LINK_CORE}/zwave_host/test/zw_host_crypto.c
)
target_include_directories(link_sim_lib PUBLIC ${CMAKE_CURRENT_LIST_DIR}
                                               ${LINK_CORE}/zwave_host/test)
# BL-216: the master's devices are home_control's registry; settings travel as proto_prefs fields.
target_link_libraries(link_sim_lib PUBLIC panel_link alarm_core zwave_host home_control alarm_proto)
if(WIN32)
    target_link_libraries(link_sim_lib PUBLIC ws2_32 bcrypt)
else()
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(link_sim_lib PUBLIC Threads::Threads)
endif()

add_executable(link_sim link_sim.c)
target_link_libraries(link_sim PRIVATE link_sim_lib)

# The seeded test port (components/core/panel_link/test): deterministic, so loopback only.
add_executable(link_smoke link_smoke.c ${LINK_CORE}/panel_link/test/plink_test_port.c)
target_include_directories(link_smoke PRIVATE ${LINK_CORE}/panel_link/test)
target_link_libraries(link_smoke PRIVATE link_sim_lib)

if(ALARM_GNU_LIKE)
    foreach(t link_sim_lib link_sim link_smoke)
        target_compile_options(${t} PRIVATE ${ALARM_WARNINGS})
    endforeach()
endif()

# LINK-013 in CI: skipped (77) where no UDP socket can be had on 127.0.0.1.
add_test(NAME link_smoke COMMAND link_smoke)
set_tests_properties(link_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
