# The event log ring on a simulated flash with power cuts (LOG-NNN, docs/features/05).
alarm_add_test(elog SOURCES test_elog.c LIBS event_log unity)
