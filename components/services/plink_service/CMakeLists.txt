# plink_service - the panel link on a panel (docs/features/11, BL-048): the role, the link task with
# its UDP socket and the role's link manager (components/core/panel_link, host-tested), the crypto
# port on PSA, mDNS, the master's listener, worker and Smart Home task (plink_dev, BL-216), the
# keypad's UI backend, sounds, time and update gate, its mirror of the master's devices and its
# settings for the master (BL-216; panel_voice: its voice's state in the report), the `plink`
# console command. Compiled only with ALARM_LINK ("absent": callers guard).
idf_component_register(SRCS "src/plink_service.c" "src/plink_psa.c" "src/plink_master_svc.c"
                            "src/plink_home_svc.c" "src/plink_keypad_svc.c" "src/plink_backend.c"
                            "src/plink_console.c"
                       INCLUDE_DIRS "include"
                       REQUIRES alarm_proto platform_esp ui_backend
                       PRIV_REQUIRES alarm_core alarm_mem console device_service esp_app_format
                                     esp_netif esp_timer ha_link i18n lwip mbedtls net_mqtt nvs_flash
                                     panel_link panel_ota panel_voice)
