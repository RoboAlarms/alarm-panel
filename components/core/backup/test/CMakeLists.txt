# The backup file (CFG-005, CFG-006, docs/features/14): records, encryption with a crypto port
# written from the standards (backup_host_crypto.c, on alarm_proto's SHA-256), the frozen
# format-1 fixture, the policy and the check before a restore.
alarm_add_test(backup STRICT SOURCES test_backup.c backup_host_crypto.c
               LIBS backup alarm_proto unity)
