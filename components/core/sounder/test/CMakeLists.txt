# The keypad sounder (AUD-NNN, docs/features/08): the patterns rendered to PCM and listened to -
# the temporal-3 and temporal-4 timings, the volume, and one-shots over a pattern.
alarm_add_test(sounder SOURCES test_sounder.c LIBS sounder unity STRICT)
# The Voice volume on speech, up to +9 dB with a limiter (AUD-033): tones and clicks through it.
alarm_add_test(speech_boost SOURCES test_speech_boost.c LIBS sounder unity STRICT)
