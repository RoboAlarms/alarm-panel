# The E32R40T application: the shared start-up (panel_app) and this board's component. What it
# carries is its manifest (../sdkconfig.defaults); the optional components follow their feature
# options (components/services/alarm_features).
idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES panel_app alarm_features board_e32r40t panel_ota)
