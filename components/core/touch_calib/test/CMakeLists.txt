# Resistive touch calibration (CYD-008) and filtering (CYD-009), docs/features/19.
alarm_add_test(touch_calib SOURCES test_touch_calib.c LIBS touch_calib unity STRICT)
