# RoboAlarms Panel firmware for the LCDWiki 4.0" ESP32-32E display (E32R40T, classic ESP32).
cmake_minimum_required(VERSION 3.22)

# The components (the tiers, and of the boards only this one), the version and the signed
# project name per profile, alarm_<board>[-dev|-release] (cmake/alarm_firmware.cmake).
set(ALARM_BOARD e32r40t)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/alarm_firmware.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(${ALARM_PROJECT_NAME})
alarm_check_build_profile()
