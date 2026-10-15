# Rules for the portable components (`alarm_core`, `event_log`, `alarm_proto`, `zone_hub`, `sounder`, `rf_decode`, `cc1101_regs`, `h264_sps`, `zwave_proto`, `zwave_host`, `zwave_zones`, `i18n`, `pack_store`, `backup`)

Rules for the portable tier (`components/core`). Claude Code loads this file by itself when it reads
a file here (through `CLAUDE.md` beside it); other agents are sent here by the root
[AGENTS.md](../../AGENTS.md) "Before you edit". Older notes cite it as CONTRIBUTING.md "Rules for the
portable components".

- Plain C11. No malloc, no globals, no RTOS or ESP-IDF headers. Time comes in as `uint64_t` ms.
  The one exception is `notify`'s hook (`notify_set_hooks`, docs/features/41 D30): set at start,
  before the tasks that read words, because those words are built on many paths that have no
  context to pass it through.
- All state lives in the caller-owned `alarm_engine_t`. Callbacks (ports) must not re-enter the engine.
- Keep the host build warning-free (`-Wall -Wextra -Wpedantic -Wconversion -Werror`).
- Behaviour follows classic keypad alarm panels and SIA CP-01. Deliberate choices are listed in
  `docs/features/01-Core-Alarm-Engine/README.md`.
- Every requirement ID (`CORE-XXX-NNN`) in that doc must have at least one test in
  `components/core/alarm_core/test/` tagged with the ID in a comment. Add or update both
  together. Other components' requirement tags live in their own `test/` folders.
- Names in `src/strings.c` are an external contract (MQTT/JSON): never rename, only add.
- `alarm_event_type_t` values are stored in the event log: append new types, never reorder.
- The configuration blob (`src/codec.c`) is stored in NVS and read by older and newer firmware:
  never reuse or renumber a field id; add new fields with new ids (missing ones keep their
  defaults); raise `compat` only when older readers would misread the blob; when the format
  changes, add a new frozen fixture next to `components/core/alarm_core/test/codec_fixture_v1.h` (never edit one).
- `components/core/event_log`: its 64-byte record layout is an external contract, new fields only in
  the reserved bytes. Tests reference `LOG-NNN` from docs/features/05.
- `components/core/alarm_proto`: topics, JSON keys and discovery ids are an external contract
  (docs/features/10): never rename, only add, and update the golden payloads in
  `components/core/alarm_proto/test/test_proto.c` with them. Tests reference `MQTT-NNN`. Every payload must fit
  `PROTO_PAYLOAD_MAX`; incoming JSON goes through the strict reader only. Firmware-update
  helpers (`proto_fw.h`) are tested in `components/core/alarm_proto/test/test_fw.c` with `OTA-NNN` tags (docs/features/13).
- `components/core/sounder`: the pattern table in `src/sounder_mix.c` is the sound of the panel, and
  the table in `docs/features/08-Audio-and-Annunciation/README.md` is the same table in words - change
  one, change the other, and change the test that measures it. `components/core/sounder/test/test_sounder.c` listens
  to the rendered PCM (tags `AUD-NNN`); the fire (temporal-3, NFPA 72) and CO (temporal-4,
  NFPA 720) timings are standards, not preferences. Hear a change in the simulator before a panel.
- `components/core/zone_hub`: `zsrc_kind_t`, `zsrc_role_t`, `zsrc_map_mode_t` and the binding blob's
  field ids (`src/bind_codec.c`) are stored in NVS (`zones`/`bind`): never renumber, only add;
  every field's zero is its default (zeros aren't written); when the format changes, add a new
  frozen fixture next to `components/core/zone_hub/test/zone_bind_fixture_v1.h`. Sources report by address through
  a `zsrc_sink_t` and never see zones or the engine. Tests in `components/core/zone_hub/test/test_zone_hub.c`
  reference `ZSRC-NNN` (docs/features/22); `zsrc_rf`'s protocol numbers (`ZSRC_RF_PROTO_*`, the same
  as `rf_proto_t`) and its address layout (id = protocol << 32 | device, input = code + 1) are
  stored in bindings too. `components/core/zone_hub/test/test_rf.c`, `test_rf_devices.c` (the mock devices of `rf_mock.c`) and `components/core/rf_decode/test/test_rf_decode.c` reference `RF-NNN`
  (docs/features/25).
- `components/core/backup`: the backup file's format (`include/backup/backup.h`: the header,
  the record and value types) is an external contract - a file one firmware writes must restore
  on a later one. Never renumber, only add; when the format changes, add a new frozen fixture
  next to `components/core/backup/test/backup_fixture_v1.h` (made independently, never edited).
  What a backup holds is the policy table in `src/backup_policy.c`: add a namespace only when an
  installer would want it back on a replacement board, never the panel's identity or keys.
  Tests reference `CFG-005`/`CFG-006` (docs/features/14).
- `components/core/i18n`: English is the source and has no table; the catalog layout in
  `include/i18n/i18n.h` (FNV-1a keys, entries sorted by hash, plural forms in CLDR category order)
  is what `tools/i18n.py build` generates, and the plural rules in `src/i18n_plural.c` are mirrored
  in `tools/i18n.py` (a tool test compares them): change one, change the other. Never edit the
  generated `catalogs/*_gen.c`; translations live in `po/<locale>.po`. A lookup allocates nothing,
  and a translation is never a `printf` format (`i18n_format` knows only `{0}`..`{9}`). Tests
  reference `I18N-NNN` (docs/features/37). `notify`'s word functions take the catalog as their
  first argument (NULL is English).
- Language packs (docs/features/37 language-packs.md): the pack file's format (`include/i18n/i18n_pack.h`, written by `tools/i18n.py pack`) and the pack store's table (`components/core/pack_store`, read by the firmware and by the Flasher's `tools/flasher/roboflash/packstore.py`) are stored contracts: a pack or a table one version writes must read on later ones. Never renumber or move a field; a format change adds a new frozen fixture next to `components/core/i18n/test/pack_fixture_v1.h` or `components/core/pack_store/test/pack_store_fixture_v1.h` (never edit one), and changes the C and the Python side together. A pack is used only after its signature and `i18n_pack_open()` pass; English never depends on one. Tests reference `I18N-039`..`I18N-049`. The asset pack file (`RAST`, docs/features/39 asset-packs.md: `components/core/pack_store/include/pack_store/asset_pack.h`, written by `tools/asset_pack.py`, read by `packstore.py` `read_asset` too) is a stored contract under the same rules, with its frozen fixture `components/core/pack_store/test/asset_pack_fixture_v1.h` (made independently, never edited); its store kinds 3-6 are never renumbered. Tests reference `OPT-012`.
