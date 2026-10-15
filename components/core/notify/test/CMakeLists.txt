# The event sentences (docs/features/26): the golden wording every event type is described with,
# the buckets the event log filters by, and silent events. No alarm engine is involved.
alarm_add_test(notify SOURCES test_notify.c LIBS notify unity STRICT)
# The gate and the reporter together, the way net_notify drives them (S02, NOTE-014): a real
# event log in RAM, a real proto_report_t and a scripted HTTP transport.
alarm_add_test(notify_delivery SOURCES test_delivery.c LIBS notify alarm_proto unity STRICT)
