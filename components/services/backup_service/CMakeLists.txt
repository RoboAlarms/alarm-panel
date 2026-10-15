# backup_service - the panel's backup file (docs/features/14 CFG-005, CFG-006): made from NVS and
# restored into it through components/core/backup's policy, sealed with PSA (PBKDF2 and
# AES-256-GCM). The Panel Manager offers it to the installer (panel_web).
idf_component_register(SRCS "src/backup_service.c" "src/backup_psa.c" "src/backup_console.c"
                       INCLUDE_DIRS "include"
                       REQUIRES alarm_core backup esp_common
                       PRIV_REQUIRES platform_esp console esp_app_format esp_timer mbedtls alarm_proto
                                     nvs_flash esp_system)
