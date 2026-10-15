# The Zigbee contracts compile, under the portable components' own warnings (docs/features/20
# testing.md §1; BL-143 A1): every header of zigbee_zones, the radio contract in zsrc_zigbee.h
# and ui_backend's zb_* block, and the facts that tie them together. The behaviour suites
# (setup, fake radio, replays) join this folder milestone by milestone.
alarm_add_test(zbz_contract SOURCES test_zbz_contract.c LIBS zigbee_zones unity STRICT)
# ZCL frames as bytes: golden frames both ways and every decode failure (testing.md §3; ZB-003,
# ZB-012, ZB-022).
alarm_add_test(zbz_zcl SOURCES test_zbz_zcl.c LIBS zigbee_zones unity STRICT)
# The device registry and what is stored, against its frozen v1 fixture (testing.md §6; ZB-010,
# ZB-021). Regenerate a NEW fixture (never edit one) with: DUMP_FIXTURE=1 build/host/test_zbz_registry
alarm_add_test(zbz_registry SOURCES test_zbz_registry.c LIBS zigbee_zones unity STRICT)
# Install codes: the CRC, typed and scanned forms, the codes waiting and their frozen v1 blob
# (devices.md §8, testing.md §9; ZB-015).
alarm_add_test(zbz_codes SOURCES test_zbz_codes.c LIBS zigbee_zones unity STRICT)
# The model table, checked whole (testing.md §5; ZB-007, ZB-023, ZB-024).
alarm_add_test(zbz_models SOURCES test_zbz_models.c LIBS zigbee_zones unity STRICT)
# zbz_panel: each device's setup, the operations and their refusals, the pages' views, what is
# stored (the cfg frozen fixture) and logged, against a scripted radio (devices.md §5-§9,
# testing.md §5-§8; ZB-010..ZB-018, ZB-021, ZB-027); sirens, keypads and fobs against the fake
# radio and the real engine (testing.md §4, §9; ZB-023, ZB-024).
alarm_add_test(zbz_panel SOURCES test_zbz_panel.c LIBS zigbee_zones_fake alarm_sim unity STRICT)
# The converter: every device signature, interviewed through the fake radio, becomes the registry
# record and the zsrc_zb_desc_t its row expects (testing.md §2; ZB-003, ZB-005, ZB-007, ZB-012).
alarm_add_test(zbz_signatures SOURCES test_zbz_signatures.c LIBS zigbee_zones_fake unity STRICT)
# The fake radio itself: real ZCL and ZDO bytes, joins and leaves, every fault knob, the sleepy
# wake window (testing.md §2, §7, §8; ZB-012, ZB-022).
alarm_add_test(zbz_fake SOURCES test_zbz_fake.c LIBS zigbee_zones_fake unity STRICT)
# Sirens and strobes as the panel's bell: the IAS WD bits and frames, the policy against a fake
# radio, supervision, and the stored assignments against their frozen v1 fixture (testing.md §4,
# §12; ZB-023).
alarm_add_test(zbz_wd SOURCES test_zbz_wd.c LIBS zigbee_zones unity STRICT)
# IAS ACE keypads and key fobs: the frames both ways, the policy against the real engine (codes,
# lockout, fobs, panics, retransmissions, the panel's state) and the frozen v1 "ace" blob
# (alarm-devices.md §3-§4, testing.md §9; ZB-024).
alarm_add_test(zbz_ace SOURCES test_zbz_ace.c LIBS zigbee_zones alarm_sim unity STRICT)
# The replays: short scripts, each one device's story, through zbz_panel, zsrc_zigbee and a real
# zone_hub over the fake radio, with virtual time - the clean, lossy and sleepy profiles, every edge
# case of testing.md §7 and the sleepy and supervision cases of §8 (ZB-003, ZB-005, ZB-010..ZB-014,
# ZB-021..ZB-024).
alarm_add_test(zbz_replay SOURCES test_zbz_replay.c LIBS zigbee_zones_fake alarm_sim unity STRICT)
# Fuzzing with fixed seeds: every decoder (ZCL frames, the registry, codes, cfg, wd and ace blobs,
# the install-code parsers, the keypads' requests) takes random and mutated input without reading
# past it, and what decodes re-encodes the same (testing.md §7 "robustness"; ZB-003, ZB-010,
# ZB-012, ZB-015, ZB-021, ZB-023, ZB-024).
alarm_add_test(zbz_fuzz SOURCES test_zbz_fuzz.c LIBS zigbee_zones unity STRICT)
