# The ESP32-S3-DevKitC-1 headless application: the shared start-up (panel_app), this board's
# component and the Panel Manager without a screen. What it carries is its manifest
# (../sdkconfig.defaults); the optional components follow their feature options
# (components/services/alarm_features).
idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES panel_app alarm_features platform_esp board_esp32s3_devkitc
                                panel_ota panel_web dev_remote console)
