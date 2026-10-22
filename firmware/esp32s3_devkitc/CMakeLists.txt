# RoboAlarms Panel firmware for the ESP32-S3-DevKitC-1 (N16R8) as a headless panel, docs/features/42.
cmake_minimum_required(VERSION 3.22)

# The components (the tiers, and of the boards only this one), the version and the signed
# project name per profile, alarm_<board>[-dev|-release] (cmake/alarm_firmware.cmake).
set(ALARM_BOARD esp32s3_devkitc)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/alarm_firmware.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(${ALARM_PROJECT_NAME})
alarm_check_build_profile()
