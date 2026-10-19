# Radio-only project (short directory for Windows' ESP-Hosted path limit).
cmake_minimum_required(VERSION 3.20)
set(IDF_TARGET esp32c6)
set(PROJECT_VER "3.0.10-ra3")
set(EXTRA_COMPONENT_DIRS "../../components/services/espnow_bridge")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(crowpanel_c6_cp)
