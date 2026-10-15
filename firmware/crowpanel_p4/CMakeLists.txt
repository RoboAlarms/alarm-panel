# RoboAlarms Panel firmware for the Elecrow CrowPanel Advanced 7" (ESP32-P4 + C6), board rev V1.1:
# the base of the other boards on the same main board (firmware/crowpanel_10).
cmake_minimum_required(VERSION 3.22)

# The components (the tiers, and of the boards only this one and its family library), the
# version and the signed project name per profile, alarm_<board>[-dev|-release]
# (cmake/alarm_firmware.cmake).
set(ALARM_BOARD crowpanel_p4)
set(ALARM_BOARD_FAMILY crowpanel_p4_family)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/alarm_firmware.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(${ALARM_PROJECT_NAME})
alarm_check_build_profile()
alarm_optimize_hot_paths()
