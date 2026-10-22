# The remote backend (docs/features/11, LINK-057..LINK-065): a keypad's proxies over a pretend
# channel straight into ui_remote_serve on a fake alarm panel's backend, the grant's rules, and
# the frozen v1 bytes.
alarm_add_test(ui_remote SOURCES test_ui_remote.c LIBS ui_remote ui_backend panel_link alarm_core
               unity INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../src STRICT)
