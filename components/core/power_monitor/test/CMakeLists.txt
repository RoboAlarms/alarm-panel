# Power supervision (docs/features/17): mains debounce, source failures and gaps, the battery and
# its low-level hysteresis, the STC battery connector's frames, power saving.
alarm_add_test(power_monitor SOURCES test_power_monitor.c LIBS power_monitor unity STRICT)
