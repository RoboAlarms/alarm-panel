# backup - the panel's backup file (docs/features/14 CFG-005, CFG-006): its records, its
# password-based encryption over a crypto port, what it holds (the policy) and the full check
# before a restore. Portable C11; components/services/backup_service makes and restores it.
set(BACKUP_SRCS
    src/backup_file.c
    src/backup_policy.c
    src/backup_check.c
    src/backup_store.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${BACKUP_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES alarm_core zone_hub
                           PRIV_REQUIRES alarm_proto) # the update server's record (S05)
else()
    add_library(backup STATIC ${BACKUP_SRCS})
    target_include_directories(backup PUBLIC include)
    target_link_libraries(backup PUBLIC alarm_core zone_hub PRIVATE alarm_proto)
    target_compile_features(backup PUBLIC c_std_11)
    set_target_properties(backup PROPERTIES C_EXTENSIONS OFF)
endif()
