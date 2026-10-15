# alarm_mem - the one memory placement policy for services (docs/features/33 step 3, ADR-037).
idf_component_register(SRCS "src/alarm_mem.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common freertos
                       PRIV_REQUIRES heap)
