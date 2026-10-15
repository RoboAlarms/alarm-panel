# automation_service - the panel's built-in automations (docs/features/41): the stored rules in
# NVS (automation's auto_store over the "automations" namespace), the save lock and the readers and
# edits the touchscreen and the Panel Manager call. Optional: ALARM_FEATURE_AUTOMATIONS; off, it
# answers with stubs (nothing stored, every edit refused).
idf_component_register(SRCS "src/automation_service.c" "src/auto_names.c"
                       INCLUDE_DIRS "include"
                       REQUIRES automation platform_esp
                       PRIV_REQUIRES nvs_flash esp_hw_support device_service console notify)
