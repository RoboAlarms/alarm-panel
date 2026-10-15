# Asset packs (docs/features/39 O3-O5, ADR-046)

Data that a panel needs but that need not sit in the firmware image moves into the pack store
(ADR-045, docs/features/37 language-packs.md "The pack store"), beside the language and voice
packs: the letters English does not use, the screen saver's photo, the full list of root
certificates, and later the Panel Manager's files. Each is a signed pack of its own kind, checked
at install and at every load, with a stated fallback when it is missing (OPT-006). The Flasher
installs them with the firmware and the panel fetches a missing or outdated one from the release
manifest, as it does a language (OPT-007).

## Decisions
- **Kinds of their own** (store kinds 3-6), one shared file format (`RAST`), so each kind keeps
  its own size bound, loader and fallback, and the store's table says what a blob is.
- **The letters (decided 2026-10-10):** both blocks leave the image - Latin
  Extended-A (Polish, Czech, Turkish, Hungarian) and the rest of Latin-1 (é, ñ, ü for the names
  people type). The image's text fonts keep ASCII, the symbols the screens use and the letters of
  the catalogs the project ships (`components/core/i18n/catalogs/i18n_chars.txt`, which
  `tools/lvpro_gen.py` already adds to every font that covers ASCII), so a shipped translation
  draws without the pack. The headline font draws only the panel's own words and needs no part
  in the pack (a language pack can still bring a headline font, I18N-049). Without the letters
  pack a letter outside that set draws as the placeholder box (I18N-011), and the keyboard has
  no àé key until the pack is loaded, the space bar taking its place (the proposal approved
  2026-10-10, UI-044; now docs/features/07 `mockup/screens/settings.js` `keyboard-without-letters`).
- **Trust follows the firmware:** the certificates pack is used only when it was made for the
  running version; otherwise the panel uses the common list built into the image until the
  matching pack is fetched. An update can never bring back an authority that the running
  firmware's list dropped.
- **The Panel Manager's files** (kind 6, O5) come after the first three work on a panel (decided
  2026-10-10).

## The pack file (format 1)
`<name>.roboasset`, exactly what `espsecure sign_data --version 2` writes, as for a language pack:
the payload padded with `0xFF` to a multiple of 4096 bytes, then one 4096-byte signature sector.
The signature covers SHA-256 of the padded payload. The store keeps the file byte for byte.

The payload, little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `RAST` (0x52 0x41 0x53 0x54) |
| 4 | 2 | format, 1 |
| 6 | 2 | header size, 128 |
| 8 | 4 | payload length in bytes (unpadded, this header included): the shared envelope's field |
| 12 | 1 | kind: the store kind it is kept as (3, 4 or 5; 6 for O5); 0x80 is reserved for the Flasher's board descriptions, which no panel keeps ("The kinds") |
| 13 | 1 | flags: bit 0 draft (made by a development build); other bits 0 |
| 14 | 2 | part count, 1-16 |
| 16 | 16 | name: the store entry's name, 1-15 printable ASCII without spaces, NUL-padded |
| 32 | 16 | firmware version it was made with (`VERSION`), printable ASCII, NUL-padded |
| 48 | 4 | match: what the firmware compares (the letters: the fonts' fingerprint; 0 for the others) |
| 52 | 4 | parts offset: 128 (the part table follows the header) |
| 56 | 72 | reserved, 0 |

A **part** record, 32 bytes, `part count` of them back to back:

| Offset | Size | Field |
|---|---|---|
| 0 | 16 | name, 1-15 printable ASCII without spaces, NUL-padded, unique in the pack |
| 16 | 4 | type: 1 an LVGL binary font (`lv_font_conv --format bin`), 2 a baseline JPEG, 3 an ESP-IDF certificate bundle (`gen_crt_bundle.py`'s format); 4 is reserved for JSON (UTF-8) in kind 0x80 |
| 20 | 4 | offset from the payload's start, a multiple of 4 |
| 24 | 4 | size in bytes, at least 1 |
| 28 | 4 | reserved, 0 |

Parts follow the part table in table order, each at the next multiple of 4, with fewer than
four zero bytes between them and after the last; the payload ends at the last part's end
rounded up to 4. Canonical layout, so one set of inputs makes one file.

**A reader refuses** (`asset_pack_open()`): a payload shorter than the header; the wrong magic,
format or header size; a payload length that differs from the bytes given; a kind outside 3-6;
unknown flag bits; a part count of 0 or over 16; a name or version that is empty, not
NUL-terminated, not printable ASCII or holds a space; a parts offset other than 128; reserved
bytes or alignment bytes not 0; a part whose name is bad or repeated, whose type is 0 or
unknown, whose size is 0, or whose bytes are not exactly where the canonical layout puts them;
arithmetic that would overflow. It never reads outside the bytes it was given, allocates
nothing, and needs the payload 4-byte aligned on a little-endian machine. The kind's own rules
come after it (below), in the kind's loader.

The reader is `components/core/pack_store/include/pack_store/asset_pack.h`:
`asset_pack_open(payload, len, &pack)` fills an `asset_pack_t` (kind, flags, draft, name,
version, match, the parts: pointers into the caller's buffer, nothing copied) or returns one of
`ASSET_PACK_ERR_ARG` (NULL, misaligned, too long, big-endian), `_SHORT` (shorter than the header or
the part table), `_MAGIC`, `_FORMAT` (format or header size), `_LENGTH`, `_KIND`, `_FLAGS`,
`_COUNT`, `_NAME`, `_VERSION`, `_PARTS_OFFSET`, `_RESERVED` (the header's or a part record's),
`_PART_NAME` (bad or repeated), `_PART_TYPE`, `_PART_SIZE`, `_LAYOUT` (a part not in its canonical
place, past the end, or a payload that does not end at the last part rounded up to 4) and
`_PADDING` (an alignment byte not 0); `asset_pack_result_name()` names each for a log. Beside it:
`asset_pack_part(pack, i)`, `asset_pack_find(pack, name)`, `asset_pack_is_draft()`,
`asset_pack_made_for(pack, version)` (an exact match: the certificates rule, OPT-013) and
`asset_pack_store_name(kind)`. Part types are `ASSET_PART_FONT`, `ASSET_PART_JPEG`,
`ASSET_PART_CERTS`; the store kinds `PS_KIND_LETTERS`, `PS_KIND_PICTURE`, `PS_KIND_CERTS`,
`PS_KIND_WEB` (`pack_store.h`). Its Python twin is `roboflash/packstore.py` `read_asset(file)`
(the envelope, then the same refusals, `StoreError` in words; it returns the name, kind, version,
match, draft, parts, payload length and the signing key's SHA-256) and `read_asset_payload()`.

## The kinds

| Kind | Store name | Parts | File at most | Fallback when missing or refused |
|---|---|---|---|---|
| 3 letters | `letters` | one LVGL binary font (type 1) per text role it covers, named by role: `caption`, `body`, `label`, `title` | 256 KB | the letter draws as the placeholder box; the keyboard has no àé key |
| 4 picture | `saver` | `photo`: a baseline JPEG (type 2), at most 1024 x 600 | 512 KB | the saver keeps its plain dark background, as when memory is short |
| 5 certificates | `certs` | `bundle`: an ESP-IDF certificate bundle (type 3), at most `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS` certificates | 128 KB | the common list built into the image (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN`) |
| 6 web (O5) | `web` | the Panel Manager's files | later | a built-in page that fetches its files |
| 0x80 Flasher board descriptions | `boards` | `boards`: the Flasher's `boards.json` (type 4), whose top level has a whole `format` of at least 1 and `boards`, an object of boards by id | 1 MB | not a panel's: a panel's reader refuses the kind; the firmware server serves the newest and the Flasher checks the signature with its own keys (BL-041) |

- **Letters.** Each part is the same typeface, size and bit depth as the built-in font of its
  role and holds exactly the code points of the font's `letters` attribute (U+00A0-U+017F, in
  `globals.xml`) that the built-in font lacks: its `range` (ASCII), its symbols and the catalogs'
  characters stay in the image, so each of the four parts has 209 code points. The headline has
  no letters font. Each of the five text fonts keeps the line it had with U+00A0-U+017F in the
  image (its `line` attribute; `lvpro_gen.py` measures the line over those code points), so the
  layout is the same with or without the pack and a letter from the pack fits its line. `tools/lvpro_gen.py --fonts` makes the parts beside the layout's fonts, as
  `fonts/letters/font_<role>.bin` (`lv_font_conv --format bin --no-compress`) with
  `fonts/letters/letters.json` (`format`, `fingerprint`, and the file of each role); the pack
  builder reads them there and the PC preview loads the same `.bin` files directly. `match` is
  the fingerprint, 32-bit FNV-1a (the i18n hash's constants) over one UTF-8 line per letters font,
  in `globals.xml` order: `<font name>|<SHA-256 of its typeface file>|<size>|<bit depth>|<1 if
  compressed, else 0>|<its letters as sorted hex ranges>`. Hashing the typeface's bytes, not its
  name, makes a changed font file change the fingerprint. The image carries the same number as
  `alarm_ui_letters_fingerprint` (generated, in `alarm_ui_gen.c`), and `lvpro_gen.py --check`
  fails when `letters.json`, that constant or a listed `.bin` disagrees with `globals.xml` and the
  typefaces. As built (2026-10-10): `0x7145e655`, from Inter Medium (caption), Inter Regular
  (body) and Inter SemiBold (label, title); the part files are 12.2, 18.9, 20.6 and 35.1 KB. A
  pack whose fingerprint differs still loads - a letter a pixel off beats a box - and counts as
  outdated, so the matching one is fetched. Parts of an unknown role are skipped.
- **Picture.** Decoded once at the saver's first showing, as the built-in photo is today
  (`ui_saver.c`, TJPGD into an RGB565 image in PSRAM). The firmware build does not link
  `saver_forest_jpg.c` (the PC backend embeds it as the part `photo`). A JPEG that does not decode,
  a NULL hook or a `false` answer means the plain background; a pack installed later (the
  generation changes) is tried once at the next preparing, never at every showing.
- **Certificates.** Loaded at start, before anything opens a TLS connection, only when its
  version equals the running firmware's (above); kept in PSRAM until restart and handed to
  `esp_crt_bundle_set()`, whose own check must pass. A newly installed pack takes effect at the
  next start. The pack is made from the list ESP-IDF ships, exactly as the image's full list
  was: the firmware build runs ESP-IDF's `gen_crt_bundle.py` with the inputs and options of
  ESP-IDF's `DEFAULT_FULL` branch (`cacrt_all.pem`; `cacrt_local.pem` only without
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`, which is on here; the deprecated list
  and a custom bundle when configured; `--max-certs CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS`).
  As built (2026-10-10, ESP-IDF 6.1): 145 certificates, a 66,984-byte bundle; the common list the
  image keeps is 15,905 bytes. `asset_packs_start()` calls `esp_crt_bundle_set()` with the
  loaded pack's bundle, which keeps the pointer (the copy stays in PSRAM until restart);
  `esp_crt_bundle_detach()` would drop it back to the image's list, and nothing in the firmware
  calls it. A bundle that fails `esp_crt_bundle_set()`'s check is `refused` and the common list
  stays.
- **The servers the panel itself reaches** must be covered by the common list, since the pack may
  be missing or made for another version (the window after an update, before the new pack is
  fetched). Checked 2026-10-10 with `openssl s_client -showcerts`, each chain's top and the root
  of the common list it reaches (through `CROSS_SIGNED_VERIFY` where the top is cross-signed):
  `firmware.zwave-js.io` (Z-Wave firmware) ISRG Root X2, cross-signed by ISRG Root X1;
  `github.com` Sectigo Public Server Authentication Root E46, cross-signed by USERTrust ECC;
  `objects.githubusercontent.com` and `release-assets.githubusercontent.com` (release downloads)
  Let's Encrypt Root YR, cross-signed by ISRG Root X1; `ntfy.sh` the same; `api.pushover.net`
  DigiCert Global Root G2, cross-signed by DigiCert Global Root CA. The project's update server
  (`https://firmware.<domain>`, Kconfig's default domain) did not answer yet; its site chains to
  ISRG Root X1. The time is SNTP, without TLS. The check needs the network, so it is not a test:
  repeat it when a server or ESP-IDF's common list changes.
- **Servers the owner chooses** (an MQTT broker, a notifier on a server of their own) may chain to
  an authority only the full list has: they connect once the matching pack is loaded, and in the
  window after an update, before the new pack arrives, they are verified against the common list
  (an MQTT broker can also be given its own authority, MQTT-001).

## The panel
- **The store:** `pack_assets` (components/services/lang_packs) stays the one owner of the
  partition; it takes kinds 3-5 with the bounds above (`PA_LETTERS_MAX_FILE`,
  `PA_PICTURE_MAX_FILE`, `PA_CERTS_MAX_FILE`). The asset service beside the language service
  (`asset_packs.h`, built with `ALARM_FEATURE_LANG_PACKS`; without it every call answers "none")
  loads each kind at start, from the start-up task right after `lang_packs_start()` and before
  `net_wifi_start()` (an internal-RAM stack, so before any TLS): finds the entry by kind and the
  kind's store name, copies it into a 4-aligned PSRAM buffer, checks the signature (`pa_verify`),
  opens it with `asset_pack_open()` and holds it to the kind's rules - the header's kind and name
  are the entry's; the letters' parts are all fonts (a role it does not know is skipped); the
  picture is exactly one `photo` JPEG, baseline (SOF0) and at most 1024 x 600 by its frame
  header; the certificates exactly one `bundle` of type certs, made for the running version; `match`
  0 except for the letters - and keeps it until restart. A draft pack on a release firmware is
  refused (as I18N-048). It never blocks the alarm task or the screen: readers take no lock (the
  offered copy is published after it is filled, and never changed or freed).
- **Each kind's state** (`asset_packs_list()`): `none` (the fallback), `ready`, `refused` (a draft
  on a release firmware, certificates made for another version, a firmware that checks no
  signatures), `damaged` (the signature, the format or the kind's rules), `needs_restart` (the
  certificates installed after the start, or a kind that loaded four copies since it), `unchecked`
  (no memory, a flash read, an update holding the check: tried again at the next start), with the
  version, `match`, the stored bytes, the draft flag and a reason in words.
- **The screen** (`ui_backend_t`, each hook optional, NULL: none): `asset_font(ctx,
  role, &data, &size)` the letters pack's font for a role, `asset_picture(ctx, name, &data,
  &size)` the picture pack's part (`photo`), `asset_generation(ctx)` changes when a loaded copy
  changes, `asset_letters_match(ctx, fingerprint)` true when the loaded letters pack's `match`
  equals the fingerprint the screen passes (`alarm_ui_letters_fingerprint`: the services tier
  cannot see the UI's generated layout, so the screen hands it in). The saver asks for its photo,
  and the text fonts for the letters font of a role. The fallback chain of a text font is: the
  built-in font, then the language's font for that role (I18N-049) when the language brings one,
  then the letters font (`ui_app.c` sets the built-in font's `fallback` and the language font's
  own; each letters font is loaded once per copy the backend hands out and kept until restart).
  A pack installed while the panel runs is taken up when `asset_generation` changes (looked at
  once a second): the fonts are hung again and the open screen is laid out again, without closing
  a sheet. The keyboard's àé key is shown only while the body font draws U+00E0 through that
  chain, checked as a keyboard opens (`ui_forms.c`), never under a finger. On the PC the preview
  and the simulator read the layout's committed parts (`fonts/letters/font_<role>.bin`, the path
  `ALARM_LETTERS_DIR` from the build; a layout without the folder has none) as a loaded pack;
  the preview's walk runs with them and ends without them (`walk_without_letters`, OPT-010,
  OPT-014), and the simulator starts without them with `ALARM_SIM_LETTERS=0`. The picture is the
  embedded JPEG on the PC, and the letters' match stays NULL there.
  `asset_packs_start()` hands a certificates pack made for this version to TLS
  (`esp_crt_bundle_set()`, "Certificates" above). The kinds' rules (names, parts, the picture's
  JPEG, the draft) are the portable `asset_pack_check_kind()`, host-tested (OPT-006).
- **Console** (development and legacy builds): `asset` lists each kind (state, version, match,
  bytes, draft, the reason) and the store's free bytes; `asset verify` checks each installed pack
  again from flash (signature, format, the kind's rules) on the language service's task.
- **Installing** by the dev link, beside the voice pack's route: `PUT /dev/v1/asset` with a whole
  signed file (8192-524288 bytes; the kind from its header, then its own bound), `panelctl asset
  install <file>`; `GET /dev/v1/asset` and `panelctl asset status [--json]` show each kind's
  state (docs/features/30). Same rules as a language pack's install: not while armed or while an
  alarm sounds; the update machinery's claim; the envelope with 0xFF padding; the format and the
  kind's rules; room for the new copy beside the working one. The letters and the picture load at
  once on the installing task and the generation changes; the certificates take effect at the next
  start (`needs_restart`). A replaced copy is never freed (a reader may hold its bytes): a kind
  loads at most four copies between restarts.
- **Fetching** (OPT-007): the update server's `assets` list (`proto_fwsrv_assets`), kept by
  `panel_ota` at each release check as it keeps `languages`, under the same sequence number
  (`panel_ota_assets`). After each check the language service's task (an internal-RAM stack) looks
  at each kind: it fetches a pack when the kind has none usable (none, damaged, or refused: a
  draft on a release image, a certificates pack made for another version), and also the
  letters when the installed pack's `match` differs from the image's fingerprint (the build's,
  from the layout's `letters.json`, as `ALARM_LETTERS_FINGERPRINT`; without one, the installed
  pack's version against the running one), and the certificates when the installed pack was made for another
  version. The letters and the certificates are taken only from an entry made for the running
  version; the picture fits every version (the running version's first). An entry the check's
  list lacks is looked for in the running version's list (`GET /api/v1/languages`, waited for at
  most 30 s, asked once per start once it answered: a server added later is read after a restart).
  One try per kind per release check, an entry this pass already
  installed never twice; never while an update downloads or the panel is locked; installed through
  `asset_packs_install` (the same checks as the dev link's install). Logged as `asset <kind>: ...`.

## The release manifest (docs/features/13)
```json
"assets": [
  {"kind": "letters", "url": "roboalarms-0.9.0-letters.roboasset", "size": 98304,
   "sha256": "<64 hex>"}
]
```
Optional, as `languages`; `kind` is `letters`, `picture`, `certificates` (later `web`); a
per-entry `version` overrides the top-level one; relative URLs resolve against the manifest's
URL; an entry without `kind` or `url`, or of a kind the firmware does not know, is skipped; a
value that is not a list (`null`, as a server may send an empty list) reads as none and never costs
the `languages` beside it. The
manifest stays unsigned: the pack is the trust anchor. The firmware server's check answer and its
`GET /api/v1/languages` answer carry the same list, read by `proto_fwsrv_assets` (the panel reads
those, not the manifest, which `proto_fw_manifest_assets` still parses in the host tests); the
server adding it is BL-208, on the server's side. No release script here writes
a manifest: the firmware server replaced it.

## The Flasher (docs/features/34)
A bundle lists its asset packs in `manifest.json` (`assets`: kind, name, version, draft, path,
sha256, size), taken by `make_bundle.py` from the first build's `assets/` folder; one signed with another
key or made for another version is refused. Older Flashers ignore the list. **Install** writes
every asset pack after the firmware's parts (no *default* flag), **update** replaces each one
whose version differs, or installs one the panel lacks (kinds 3-5 only; any other entry stays as
it is). A draft pack is refused for a release image; a pack that does not fit is skipped and
said, the firmware stays installed. `roboflash/packstore.py` reads `RAST` too, checked against the same frozen
fixture as the C reader. From the firmware server (docs/features/34 FLS-032) the Flasher takes a
release's asset packs from its `GET /api/v1/languages` answer (`assets`), each checked by SHA-256,
signature and version like a bundle's, and newer board descriptions (kind 0x80) from
`GET /api/v1/flasher` (FLS-034).

## Building packs
`python tools/asset_pack.py build --kind letters|picture|certificates --version <VERSION> --out
<dir> [--key <pem>] [--draft] [--match 0x...] --part NAME=TYPE:PATH [--part ...]` (TYPE `font`,
`jpeg` or `certs`; the parts in table order) writes `<store name>.roboasset` (`letters`, `saver`,
`certs`), signed with `python -m espsecure sign_data --version 2 --keyfile <pem>` when a key is
given, else with an all-0xFF signature sector. It reads its own output back with `read_asset`
before writing it and checks the kind's rules: the size bound, the letters' parts named by role
and of type font, the picture's one `photo` of type jpeg, the certificates' one `bundle` of type
certs, `match` 0 except for the letters. `--kind boards` (TYPE `json`) makes the Flasher's
`boards.roboasset` from `tools/flasher/roboflash/boards.json`, its version the file's own, signed with
the release key (`asset_pack.py`'s docstring has the command). `asset_pack.py info <file>` prints what `read_asset`
reads. The firmware build runs it
into `<build>/assets/` with the image's signing key when the pack store is on and binaries are
signed (`cmake/alarm_firmware.cmake` `alarm_asset_packs`, version `PROJECT_VER`, `--draft` unless
`ALARM_BUILD_RELEASE`), from committed inputs: `letters.roboasset` from the board's layout's
`fonts/letters/` (`letters.json`'s fingerprint as `--match`, each role's `.bin` as a part in the
order caption, body, label, title; a layout without `letters.json` gets none), which
`tools/lvpro_gen.py` makes beside the C fonts (`--check` in CI); `saver.roboasset` from
`components/ui/alarm_ui/images/saver_forest.jpg`; `certs.roboasset` from ESP-IDF's
`gen_crt_bundle.py`, run as for the image's full list (above), as its one `bundle` part. As built
(2026-10-10, 1024x600): letters 94208 bytes (four parts, 87064-byte payload), saver 131072
bytes, certs 73728 bytes (one 66984-byte part). Keeping the common list took 50,848 bytes off the
image's read-only data. The signed image file grows and shrinks in 64 KB steps (its segments are
padded to the flash pages), so a saving shows in the file's size once the savings cross a step.

## Tests
| What | Where | IDs |
|---|---|---|
| The format: the frozen fixture (the tool reproduces its bytes), every truncation, a mutation of every header and part field, layout gaps and overlaps, unknown kinds, types and flags | `components/core/pack_store/test/test_asset_pack.c` (fixture `asset_pack_fixture_v1.h`, made independently, never edited) | OPT-012 |
| The writer and the Python reader: the fixture's bytes, round trips, refusals | `tools/tests/test_asset_pack.py` | OPT-012 |
| The kinds' rules: a letters pack of another fingerprint, a certificates pack of another version, a draft on a release firmware | host tests of the loaders' portable parts | OPT-006, OPT-013 |
| The fonts' fallback chain, with and without the letters and a language font | the preview's walk with an accented zone name, its glyph check | OPT-010, OPT-014 |
| The manifest's `assets` | `components/core/alarm_proto/test/test_fw.c` | OPT-007 |
| The Flasher: a bundle with asset packs, install and update | `tools/tests/test_flasher_*.py` with the fake chip | OPT-007 |
| On a panel: the saver, a name with an accented letter with and without the letters pack, TLS to a server whose authority only the full list has, with and without it, an update that brings the packs along | BL-124 | OPT-006, OPT-007, OPT-013 |
