# RoboAlarms Panel firmware for the Elecrow CrowPanel Advanced 10.1" (ESP32-P4 + C6), board rev V1.2.
# The same main board as the 7": its firmware is the 7"'s (ALARM_BOARD_BASE crowpanel_p4) with
# this board's component and the settings that differ (sdkconfig.defaults here).
cmake_minimum_required(VERSION 3.22)

set(ALARM_BOARD crowpanel_10)
set(ALARM_BOARD_FAMILY crowpanel_p4_family)
set(ALARM_BOARD_BASE crowpanel_p4)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/alarm_firmware.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(${ALARM_PROJECT_NAME})
alarm_check_build_profile()
alarm_optimize_hot_paths()
