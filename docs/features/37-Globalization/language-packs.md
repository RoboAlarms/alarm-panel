# Language packs (docs/features/37, ADR-045)

**Status:** Implemented in software (2026-09-30): the pack format and its reader, the pack store,
the `lang_packs` service, the screen, the Panel Manager, the release manifest and downloads, the
Flasher, fonts in a pack; built, reviewed and tested on the PC. Left: on a panel (BL-121).

The P4 image used to carry every translation: the Spanish draft alone was 140 KB of the image,
and every further language would have added about as much (a Chinese font 1-2 MB). English stays
in the image - it is the source text and needs no table - and every other language becomes a
**language pack**: one signed file per language, installed after the firmware into the `assets`
partition, loaded into PSRAM when the panel speaks it. The lookup code does not change: a loaded
pack is an `i18n_catalog_t` whose `entries` and `strings` point into the loaded file.

The requirements are I18N-039 to I18N-049 in [README.md](README.md); the tasks are phase LP
there.

## Decisions
- **English is always there.** A panel with no pack, a damaged pack, an unsigned pack or a pack
  a release firmware refuses speaks English, entry by entry where a pack lacks one. The alarm
  never waits on a pack: packs load after the engine starts, off the alarm task, and a failure
  is logged, never fatal. The stored language choice is kept when its pack is missing, so the
  panel returns to it when the pack is installed again (I18N-042).
- **Signed with the firmware's key.** A pack decides every word on the screen - a tampered one
  could label *Disarm* as *Armed*. The build signs it the way it signs the image (Secure Boot V2
  signature sector, `espsecure sign_data --version 2`, the build's `secrets/ota_signing_key.pem`),
  and the panel checks that signature with the key the running firmware was signed with
  (`esp_secure_boot_verify_sbv2_signature_block`, which trusts the running app's key digest on a
  panel without eFuse secure boot) - at install and again at every load, before the pack is
  parsed. Then the parser checks every offset anyway (I18N-040, I18N-041).
- **A pack store, not a filesystem.** The `assets` partition (2.5 MB, already in every P4
  partition table as `data,littlefs`; never mounted as LittleFS) holds a small **pack store**: two
  table sectors (A/B) and whole signed blobs in the sectors behind them. A pack is written once
  and replaced whole, so a filesystem's machinery buys nothing: the store costs a few KB of code
  where LittleFS would cost 20-30 KB, the Flasher writes a pack with esptool without mounting
  anything, and the A/B table makes every change power-safe. Voice libraries (docs/features/08)
  become blobs of their own kind in the same store. The partition table does not change (a
  change needs a full erase).
- **Copied into PSRAM when used.** A loaded pack is read through the flash cache
  (`esp_partition_mmap`, safe from any task), copied into PSRAM, verified and parsed there, and
  never freed or changed until the panel restarts - so a pointer another task holds stays valid
  when a pack is replaced or removed. At most `LP_MAX_LOADED` (8) loads per boot; after that a
  newly installed pack is used from the next restart.
- **Made with the firmware.** The firmware build makes each language's pack for its own version
  and signs it with the same key (`tools/i18n.py pack`); the release puts them beside the images
  and lists them in the release manifest; the Flasher's bundle carries them.

## The pack file (format 1)
`<locale>.robolang`, exactly what `espsecure sign_data --version 2` writes: the **payload**,
padded with `0xFF` to a multiple of 4096 bytes, then one 4096-byte **signature sector** (up to
three signature blocks, the rest `0xFF`). The signature covers SHA-256 of the padded payload. The
store keeps the file byte for byte.

The payload, little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `RLNG` (0x52 0x4C 0x4E 0x47) |
| 4 | 2 | format, 1 |
| 6 | 2 | header size, 128 |
| 8 | 4 | payload length in bytes (unpadded, this header included) |
| 12 | 16 | locale: a BCP 47 tag, ASCII letters, digits and `-`, 2-15 characters, NUL-padded (`es-US`) |
| 28 | 32 | the language's name in itself, UTF-8, 1-31 bytes, NUL-padded (`Español`, `English (UK)`) |
| 60 | 1 | plural rule (`i18n_plural_rule_t`, 0-5) |
| 61 | 1 | flags: bit 0 draft (a translation not yet reviewed, I18N-005); other bits 0 |
| 62 | 2 | reserved, 0 |
| 64 | 4 | entry count |
| 68 | 4 | entries offset (from the payload's start, a multiple of 4) |
| 72 | 4 | strings offset |
| 76 | 4 | strings size |
| 80 | 4 | template fingerprint: which English it was made from (below) |
| 84 | 4 | missing: entries of that template it leaves in English that it should not (0 for a variant of English) |
| 88 | 16 | firmware version it was made with (`VERSION`), ASCII, NUL-padded (`0.7.0`) |
| 104 | 4 | fonts offset, 0 without fonts |
| 108 | 4 | fonts size, 0 without fonts |
| 112 | 16 | reserved, 0 |

- **Entries:** `entry count` records of 8 bytes, `{uint32 hash, uint32 text}` - exactly
  `i18n_entry_t` (hash `i18n_hash(ctx, english)`, text = offset into the string block with the
  `I18N_ENTRY_WEB` and `I18N_ENTRY_PLURAL` flags), strictly ascending by hash.
- **Strings:** the string block. An entry's text is NUL-terminated; a plural entry has one form
  per category of its plural rule (`i18n_plural_forms()`), back to back, each NUL-terminated.
  The same bytes `tools/i18n.py build` puts in a C table (`catalog_layout()`).
- **Fonts** (optional, I18N-049): `uint32 count` (1-8), then `count` records of 24 bytes
  `{char role[16] NUL-padded, uint32 offset, uint32 size}` - role one of `caption`, `body`,
  `label`, `title`, `headline` (the 7" text fonts, docs/features/07) - each an LVGL binary font
  (`lv_font_conv --format bin`) at a 4-aligned offset inside the payload.

**The template fingerprint** is 32-bit FNV-1a (the `i18n_hash` constants) over the template's
key hashes (`messages.pot`, every entry), sorted ascending, each as 4 bytes little-endian. The
build writes the firmware's own into `catalogs/i18n_template_gen.c`
(`i18n_template_fingerprint`, `i18n_template_count`). When a pack's fingerprint equals the
firmware's, `missing` is exact: "3 phrases show in English". When it differs the pack was made
for other firmware: phrases whose English changed show in English - the panel says "Made for
version 0.6.0: some phrases may show in English" and offers the matching pack (I18N-043).

**A reader refuses** (`i18n_pack_open()`, I18N-040): a payload shorter than the header; the wrong
magic, format or header size; a payload length that differs from the bytes given; a locale that
is not 2-15 of `[A-Za-z0-9-]` or not NUL-padded; a name that is empty, not NUL-terminated or not
valid UTF-8; a plural rule over 5; unknown flag bits; reserved bytes not 0; an entries or strings
region (or the fonts region) outside the payload, overlapping the header or another region, or
not 4-aligned (entries); arithmetic that would overflow; hashes not strictly ascending; a text
offset outside the string block; a text or plural form not NUL-terminated inside the block or
not valid UTF-8; a version that is not printable ASCII and NUL-terminated; a fonts table with 0
or more than 8 records, an unknown or repeated role, or a font outside the payload. It never
reads outside the bytes it was given, allocates nothing and needs the payload 4-byte aligned on
a little-endian machine (it says so otherwise).

## The pack store (format 1)
In the partition labelled `assets`, found by label whatever its subtype; 4096-byte sectors.
Sectors 0 and 1 are the table's two copies (A and B); sectors 2 and up hold blobs.

A table sector, little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `RAPS` (0x52 0x41 0x50 0x53) |
| 4 | 2 | format, 1 |
| 6 | 2 | entry size, 64 |
| 8 | 4 | sequence: the copy with the higher one is current |
| 12 | 2 | entry count, 0-63 |
| 14 | 2 | reserved, 0 |
| 16 | 4 | the partition's sectors when written |
| 20 | 8 | reserved, 0 |
| 28 | 4 | CRC-32 (ISO-HDLC, as zlib's `crc32` and `alarm_crc32`) over bytes 0-27 then the entries |
| 32 | 64 x count | entries |

The rest of the sector is `0xFF`. An entry:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | kind: 1 a language pack; 2 reserved for a voice library; 3 letters, 4 picture, 5 certificates, 6 web (asset packs, docs/features/39 [asset-packs.md](../39-Optimizations/asset-packs.md)); others kept as they are |
| 1 | 1 | flags: bit 0 *default* - the language a panel starts in when none is stored (at most one language has it); other bits kept |
| 2 | 2 | reserved, 0 |
| 4 | 4 | first sector (2 or more) |
| 8 | 4 | length: the blob as stored (a multiple of 4096) |
| 12 | 4 | payload length (the blob's own, before padding and signature) |
| 16 | 32 | SHA-256 of the whole stored blob |
| 48 | 16 | name: ASCII, NUL-padded - a language pack's locale |

**Mounting** reads both table sectors and never writes. A copy is valid when its magic, format,
entry size, count and CRC are right, its sector count equals the partition's, and every entry
fits the partition (first sector 2 or more, length a non-zero multiple of 4096, end inside the
partition), has a NUL-terminated name and overlaps no other entry. Both valid: the higher
sequence is current (equal: A). One valid: that one. Neither: an empty store, sequence 0 - a new
panel's partition may hold anything, even the vendor's demo, and is only overwritten sector by
sector as blobs are written.

**Writing** is power-safe (I18N-044) by two rules: a blob is written only into sectors that no
entry of the *current* table covers, and a table only into the copy that is *not* current. A new
blob: find the lowest run of free sectors long enough (first fit), erase and write it, check it
(the service verifies the signature from flash; the Flasher compares MD5), then write the new
table - the current entries, the new one replacing an entry of the same kind and name, flags
kept - with sequence + 1 into the other copy. Removing an entry or changing a flag is a new table
the same way. A cut at any moment leaves either the old table with every blob it names untouched,
or the new one complete. Entries of a kind this firmware doesn't know, and unknown flag bits, are
carried into every new table unchanged.

A table write that reports a failure may still have landed (a timeout after the bytes were
written), so after any failed erase or write the store reads both copies again and carries on
from whichever is current - a failed commit can leave the *new* state, and a caller looks at the
store (`ps_find`) rather than assuming the old one. If that read fails too, the store refuses
every write until it is mounted again. A table can never hold two entries of the same kind and
name (such a copy is invalid); two with the *default* flag in one kind read as the first. A
current sequence of `0xFFFFFFFF` (only a foreign writer makes one) restarts at 1 in the other copy,
then the old copy is erased: a cut between the two leaves the old table current.

## The panel (`components/services/lang_packs`)
- **Start:** `lang_packs_start()` after the engine, before the screen: finds `assets` (none: the
  service stays off and the panel has English and any built-in catalogs), mounts the store, and
  loads the stored language's pack. With no language stored yet (a new panel, or after a factory
  reset) it adopts the store's *default* language and saves it as the preference.
- **Loading:** `lang_packs_catalog(locale)` never blocks and never touches flash: it returns the
  newest loaded pack for the locale (lock-free), else the catalog built into the image
  (`i18n_catalog()`, usually none on the P4), else NULL - English. For a pack that is installed but
  not loaded it also queues a load on the service's task and the generation changes once it is
  loaded, so the caller looks again. A load - on that task, on the start-up task for the stored
  language, or on the task that just installed the pack, each with an internal-RAM stack, because
  the signature check reads flash with the cache off (`alarm_mem_task_create` stacks are in PSRAM
  and must not) - maps the blob (`esp_partition_mmap`), copies it into a 4-aligned PSRAM buffer,
  checks its SHA-256 over the padded payload against the signature sector with
  `esp_secure_boot_verify_sbv2_signature_block()`, parses it with `i18n_pack_open()` and registers
  it in an append-only table of loaded packs whose count is published last (a release store), so
  readers need no lock. A draft pack on a release firmware (`CONFIG_ALARM_BUILD_RELEASE`) is
  refused (I18N-048).
- **Listing** (`lang_packs_list()`): every language pack in the store, read from its header
  through the mapping (locale, name, version, draft, missing, fingerprint), with whether it is
  loaded, refused or damaged (its signature or format failed when loaded).
- **Installing** (`lang_packs_install(file, len, ...)`): the whole file in PSRAM (at most
  `LP_MAX_FILE`, 2 MB): its size a multiple of 4096 and at least 8192; the signature checked, the
  payload parsed; refused if it is a draft on a release firmware or its locale is plain English
  (`en`, `en-US`); then written to the store, verified again from flash
  (`esp_secure_boot_verify_signature()`), committed, and loaded if the panel speaks it. Removing
  (`lang_packs_remove(locale)`) commits a table without it; a loaded copy stays in memory, hidden,
  until restart. Both run only on tasks with an internal-RAM stack (the Panel Manager's server
  task, the service's own task), under the service's mutex: flash writes disable the cache
  (CONTRIBUTING.md, docs/bugs/B001). Not while any partition is armed or an alarm sounds
  (`alarm_snapshot_locked()`), like every other change.
- **Downloading** (I18N-047): the release manifest's `languages` (docs/features/13) are kept by
  `panel_ota` at each release check. The service's download task fetches a listed pack
  (`panel_ota_download()`: redirects, the certificate bundle, size and SHA-256 checked) when the
  panel's language is not installed, when an installed pack was made for another version and the
  manifest has the one for the running version (this is how languages follow an update), or when
  someone asks (the Panel Manager, the console). One try per language per release check; never
  while an update downloads.
- **Generation:** `lang_packs_generation()` changes with every install, removal and load, so the
  screen and the Panel Manager know to look again.
- **Console** (development and legacy builds): `lang` lists, `lang load <locale>`,
  `lang rm <locale>`, `lang fetch <locale>`, `lang verify` re-checks every installed pack.
- **Options** (the "Alarm features" menu): `ALARM_FEATURE_LANG_PACKS` (on where PSRAM is) and
  `ALARM_I18N_BUILTIN` (catalogs built into the image; off when packs are on). A firmware without
  built-in catalogs defines `I18N_NO_BUILTIN` for `components/core/i18n`, whose generated table is
  then empty and whose catalogs are left out of the image (`cmake/alarm_firmware.cmake`).

## The screen (`ui_backend_t`)
An optional set, NULL on the PC (the preview and the simulator keep their built-in catalogs):
`lang_catalog(ctx, locale)`, `lang_list(ctx, out, max)` (a `ui_lang_t` per installed language:
locale, name, version, draft, refused, whether it matches this firmware, missing) and
`lang_generation(ctx)`, and for fonts `lang_font(ctx, locale, role, &data, &size)`. The language
list shows English and the installed languages, each with its state in words: a draft; "3
phrases show in English"; "Made for version 0.6.0: some phrases may show in English"; "Can't be
used here" for a refused one. A stored language that is not installed shows English, and the
Language row says so. When the generation changes the screen looks its language up again.

## The Panel Manager (docs/features/31)
| Request | Who (docs/features/14) | What |
|---|---|---|
| `GET /api/v1/languages` | anyone signed in | the current language, this firmware's version, the store's free space, the installed languages with their state, the languages the update source offers, the last download's state |
| `PUT /api/v1/languages` (the body is the file) | a system or partition master, the installer (as for the language itself) | install or replace a pack; not while armed or in alarm |
| `POST /api/v1/languages` `{"action": "remove", "locale": "es-US"}` | the same | remove it; the panel speaks English if it was its language |
| `POST /api/v1/languages` `{"action": "download", "locale": "es-US"}` | the same | fetch it from the update source (the answer comes back through `GET`) |

Answers carry `result`, one of `lang_result_name()`'s words (`ok`, `signature`, `format`, `draft`,
`english`, `no_space`, `locked`, `flash`, `not_found`, `busy`, `no_source`, `size`, `off`), which
the page puts into words.

`/i18n/<locale>.json` is made from the loaded pack, and its ETag now includes the pack's SHA-256,
so a new pack is fetched without a firmware change. The Settings > Language page lists the
installed languages with Remove, a file picker for a `.robolang`, and Download for those the
update source offers.

## The release manifest (docs/features/13)
```json
"languages": [
  {"locale": "es-US", "url": "roboalarms-0.7.0-es-US.robolang", "size": 155648,
   "sha256": "<64 hex>"}
]
```
Optional; a per-entry `version` overrides the top-level one; relative URLs resolve against the
manifest's URL; an entry without `locale` or `url` is skipped. The manifest stays unsigned: the
pack is the trust anchor (OTA-002).

## The Flasher (docs/features/34)
- A bundle lists its packs in `manifest.json` (`languages`: locale, name, version, draft, path,
  sha256, size) beside the images; `make_bundle.py` takes them from the first build's `lang/`
  folder and refuses one signed with another key than the images or made for another version.
  Older Flashers ignore the list (bundle format 1 stays).
- **Install** asks for the panel's language (English, or a pack in the bundle - a draft only for
  a development or legacy image). After the firmware's parts it reads the store's two table
  sectors, writes the pack into free sectors, checks the MD5, then writes the new table with the
  pack flagged *default*, so the new panel starts in that language.
- **Update** brings every installed language to the bundle's version: a language pack whose
  version differs is replaced by the bundle's pack for the same locale; flags are kept.
- `roboflash/packstore.py` is the store's format in Python, checked against the same frozen
  fixture as the C code.

## Building packs
`python tools/i18n.py pack --version <VERSION> --out <dir> [--key <pem>] [--release]
[--locale <tag> ...]` writes `<locale>.robolang` for every `.po` (a release leaves drafts out),
signed with `python -m espsecure sign_data --version 2 --keyfile <pem>` when a key is given. The
firmware build runs it (`cmake/alarm_firmware.cmake`, when `ALARM_FEATURE_LANG_PACKS` is on) into
`<build>/lang/` with the signing key the image uses. CI builds with a throwaway key, as for the
images.

## Known limits (2026-09-30, from the reviews)
- A loaded pack and a remembered bad pack are keyed by the SHA-256 in the store's table, which nothing
  compares with the bytes loaded; only a writer other than the panel and the Flasher could make them differ.
- The list counts a pack as usable from its header before its signature has been checked (a new panel can
  adopt it as its default language; the Panel Manager offers it); its first load then checks the signature,
  and a failure means English, as for any bad pack.
- A pack's fonts are loaded once per language and role and kept until restart, so a replaced pack's new
  fonts draw from the next restart; its words change at once.
- The service task's stack (10 KB, internal RAM) runs a TLS download, an install and an RSA check in one
  chain: its headroom is printed by `lang` and is measured on the bench (BL-121).

## Tests
| What | Where | IDs |
|---|---|---|
| The pack format: the frozen fixture made by the tool, every truncation, a mutation of every header field, unsorted and duplicate hashes, offsets out of range, missing NULs, bad UTF-8, plural forms short, fonts table | `components/core/i18n/test/test_i18n_pack.c` | I18N-040, I18N-043 |
| The writer: bytes of the fixture, round trip against `catalog_layout()`, drafts left out of a release, the fingerprint | `tools/tests/test_i18n*.py` | I18N-039, I18N-040 |
| The store: the frozen table fixture, mount of every combination of valid and damaged copies, first fit, replace and remove, a power cut after every erase and write, unknown kinds and flags carried | `components/core/pack_store/test/test_pack_store.c`; `tools/tests/test_flasher_packstore.py` (same fixture) | I18N-044 |
| The manifest's languages | `components/core/alarm_proto/test/test_fw.c` | OTA-0xx, I18N-047 |
| The screen: English, a draft, a pack for other firmware, a missing pack; the English walk unchanged | `components/core/ui_backend/test`, the preview | I18N-042, I18N-043 |
| The Panel Manager: who may install and remove, locked while armed, the page | `panel_web/test/*.test.cjs` with the mock | I18N-046 |
| The Flasher: install with a language, update refreshing one, a pack from another key refused | `tools/tests/test_flasher_*.py` with the fake chip | I18N-045 |
| On a panel: a pack installed by each path, a restart, an update, a damaged pack, a removed one | BL-121 | I18N-041, I18N-042, I18N-045 to I18N-047 |
