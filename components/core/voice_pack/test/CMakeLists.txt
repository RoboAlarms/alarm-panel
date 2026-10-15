# The RVOC voice pack reader (AUD-026, AUD-013, AUD-024, docs/features/08): the frozen format 1
# fixture, every rejection reason on a patched copy, the accessors and the spoken-name budget (D02).
alarm_add_test(voice_pack SOURCES test_voice_pack.c LIBS voice_pack alarm_proto unity STRICT)
alarm_add_test(voice_pack_words SOURCES test_vp_words.c LIBS voice_pack alarm_proto unity STRICT)
# What the panel accepts and the pack's immutable identity record (AUD-024, AUD-027, D08/D12).
alarm_add_test(voice_pack_accept SOURCES test_vp_accept.c LIBS voice_pack alarm_proto unity STRICT)
