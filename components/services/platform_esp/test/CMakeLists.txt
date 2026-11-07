# platform_esp's portable pieces on the PC.
# SEC-012 (docs/bugs/B033): the TLS handshake ration every listener shares.
alarm_add_test(tls_gate
               SOURCES test_tls_gate.c ${CMAKE_CURRENT_LIST_DIR}/../src/tls_gate.c
               LIBS unity INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../include STRICT)
# F2: when the service console may factory-reset the panel.
alarm_add_test(reset_policy
               SOURCES test_reset_policy.c ${CMAKE_CURRENT_LIST_DIR}/../src/reset_policy.c
               LIBS unity INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../include STRICT)
