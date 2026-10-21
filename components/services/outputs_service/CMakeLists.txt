# outputs_service - the panel's own sirens and outputs (docs/features/16): the wired outputs on the
# board's terminals and the 433 MHz sirens (components/core/sirens), with their task, store and the
# `out` console command. Optional: ALARM_OUTPUTS; off, every call answers "not supported" (stubs).
idf_component_register(SRCS "src/outputs_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES alarm_core sirens
                       PRIV_REQUIRES platform_esp rf_service console esp_driver_gpio nvs_flash)
