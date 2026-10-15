# h2_rcp - the ESP32-H2's own firmware: an 802.15.4 radio co-processor (RCP) that the panel's
# Zigbee stack drives over the wireless socket's UART (docs/features/20). It is ESP-IDF's
# OpenThread RCP (examples/openthread/ot_rcp) with the transport fixed to that UART and nothing
# else; the panel's firmware keeps every Zigbee decision. Not a panel board: it has no board
# component and no signed alarm image (tools/check_boards.py looks only at firmware/<board>/).
cmake_minimum_required(VERSION 3.22)

# Its own version, not git describe: bumped by hand when its software changes (README.md). The
# Flasher's bundle records it beside the panel's images (FLS-015).
set(PROJECT_VER "1.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only main and what it requires: the image stays small and the build fast.
idf_build_set_property(MINIMAL_BUILD ON)
project(roboalarms_h2_rcp)
