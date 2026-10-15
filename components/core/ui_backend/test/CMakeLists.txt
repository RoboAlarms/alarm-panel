# The UI's backend contract is self-contained and portable (ARCH-005, docs/features/33 step 5).
alarm_add_test(ui_backend SOURCES test_ui_backend.c LIBS ui_backend unity STRICT)
# The Smart Home screen's layout blob (a stored format) and state words (docs/features/24, BL-045).
alarm_add_test(ui_dev_model SOURCES test_ui_dev_model.c LIBS ui_backend unity STRICT)
# The touchscreen's voice approval against the engine and the pack (docs/features/08
# voice-first-build.md D07), with voice_pack's test pack writer.
alarm_add_test(ui_voice SOURCES test_ui_voice.c LIBS ui_backend alarm_sim alarm_proto unity STRICT
               INCLUDES ${ALARM_COMPONENTS_DIR}/core/voice_pack/test)
