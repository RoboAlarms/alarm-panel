# The voice's request policy (AUD-008, AUD-010, AUD-012, AUD-015, WALK-016, docs/features/08 and
# 40): every D05 sequence, every suppression, tone tickets, the queue's limits and cancellation.
alarm_add_test(annunciator SOURCES test_annunciator.c LIBS annunciator voice_pack alarm_core unity STRICT)
