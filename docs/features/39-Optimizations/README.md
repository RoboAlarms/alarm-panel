# 39 · Optimizations (firmware size, storage and leftovers)

**Status:** In progress - audited 2026-09-30 and measured again 2026-10-02, after the touchscreen
redesign. Done 2026-10-02: the size report (O0), the build settings F1, F12, F15-F17 (O1), the
fonts compressed (F2), `-msave-restore` (F9), icons per size (F6) and the Panel Manager's files
minified (F7) (O2), the generator's output made compact (F10); F11 was already so. The v1 default
image went from 65.4 % to 58.4 % of its slot. Done 2026-10-10: the asset packs O3 and O4 (the
letters, the saver photo and the full certificate list as signed packs, [asset-packs.md](asset-packs.md)):
the release image 73.7 % -> 66.8 %, dev 79.3 % -> 73.7 %, seen working on a panel. Left: their
remaining panel checks, F18/F19, F13 and the leftovers (BL-122 to BL-126; decision BL-123). Held
back until more space is needed: the web pack (O5), the Wi-Fi link's tables and a speech-only
voice (Findings) · **Milestone:** M23 ·
**Depends on:** 37 (language packs, ADR-045), 13, 31, 34

## Goal
Make the firmware smaller and tidier without giving up anything it does. The P4 image fills
65.4 % of its 4,608 KiB OTA slot (v1, default profile, 2026-10-02; CI allows 85 %), up from
51.5 % on 2026-09-25: Z-Wave joined every image, then the translations and the fonts for their
letters, then the redesigned touchscreen, the Smart Home screen and the Panel Manager's new
pages. The translations have already moved out (language packs, ADR-045). This feature finds
where the rest goes, stores data better, moves data that need not be in the image into the pack
store, fixes what is wasteful in our own code, and sweeps out code that changed features left
behind.

The rules for it (2026-09-30, 2026-10-02):
- **Every feature stays.** Z-Wave is a core feature and stays in every P4 image: making it
  optional is not an option.
- **The image ships English only.** Every other language, and what only other languages need,
  comes as a pack.
- Work on how things are stored and on the code itself, not on what the panel can do.
- Leftovers from features that changed get their own sweep, later (O7).

The aim is to be back under 60 % of the slot with O1-O2 and near 52 % with the asset packs, so
that Zigbee (5.6 % of the slot, most of it Espressif's closed library) and the features still to
come fit with room to spare. The ADR is ADR-046 (docs/features/00).

## Reference behaviour
Not applicable: this is about how the firmware is built and stored, not about how the panel
behaves. Nothing a person sees or hears changes, except where a task says so.

## Where the space goes (2026-10-02)
Measured from `build_v1` (default profile, the code of `main` 3fbec62): 3,084,288 bytes signed,
65.4 % of the slot; the dev build, which now carries Zigbee like every P4 image (ADR-047), is 3,346,432 bytes,
70.9 %. KB here is 1,000 bytes. **The image grows in 64 KiB steps:** it is padded to a 64 KiB
boundary before the signature sector goes on, so its share of the slot moves in 1.4 % jumps,
and a cut shows only once it adds up to a step.

| What | KB | % of slot | Of it: code / data / text |
|---|---:|---:|---|
| ESP-IDF itself: chip support, drivers, FreeRTOS | 393 | 8.3 | 296 / 22 / 74 |
| LVGL and its ESP port | 279 | 5.9 | 257 / 19 / 2 |
| Networking: lwIP 110, mDNS 38, HTTP, MQTT, JSON | 249 | 5.3 | 207 / 7 / 35 |
| Panel services: alarm task, console, updates, MQTT, Home Assistant, notifications, RF | 239 | 5.1 | 182 / 6 / 51 |
| Crypto and TLS | 205 | 4.3 | 172 / 19 / 15 |
| Hand-written screens (all but Z-Wave's) and their English words | 205 | 4.3 | 153 / 6 / 46 |
| Text and digit fonts: Inter, 5 sizes, clock and keypad digits | 199 | 4.2 | data |
| Z-Wave: library, service, zones | 178 | 3.8 | 159 / 3 / 15 |
| Wi-Fi link to the C6 (Espressif's message tables 117) | 174 | 3.7 | 53 / 70 / 52 |
| Screen-saver photo (1024x600 JPEG) | 126 | 2.7 | data |
| Panel Manager files (gzip) | 124 | 2.6 | data |
| Alarm logic: engine, zones, MQTT JSON, event words, sounder, log, language code | 111 | 2.3 | 97 / 5 / 8 |
| Icon fonts, 4 sizes (compressed) | 89 | 1.9 | data |
| C library, console library, NVS | 77 | 1.6 | 67 / 4 / 6 |
| Panel Manager server | 73 | 1.5 | 55 / 4 / 13 |
| SD card | 68 | 1.4 | 53 / 5 / 10 |
| TLS root certificate list (full) | 68 | 1.4 | data |
| Screens generated from the layout XML | 68 | 1.4 | 60 / 2 / 5 |
| Z-Wave screens (hand-written) | 65 | 1.4 | 46 / 0 / 19 |
| Remote screen video (H.264, JPEG) | 28 | 0.6 | 21 / 2 / 5 |
| **With Zigbee:** Espressif's Zigbee library 195, the radio link 48, our service and host 15 | +264 | +5.6 | 234 / 6 / 23 |

Since 2026-09-30 (62.6 % after F2 and F9): the hand-written screens grew from 166 to 205 KB,
the generated ones from 48 to 68 and the Panel Manager's files from 94 to 124; the rest is in
the services behind them. 131 KB in all, two steps.

How it was measured: the link map read per input section; the linker's merged string pool
(357 KB) charged back to each object by its size before merging (the map's "size before
relaxing"), since the size tool charges the whole pool to one object. O0 turns the scripts into
a tool.

### The other languages (checked 2026-10-02)
No translation is in the image: with `CONFIG_ALARM_I18N_BUILTIN` off the built-in catalog table
links as 8 bytes, the Spanish and British catalogs are compiled but not linked, and the Panel
Manager's translated words come from the loaded pack. The language code itself is small and
stays (lookup, plurals, dates and regions 9 KB, the pack loader 12, the pack store 3). **What
other languages still cost in the image is their letters:** each of the five text fonts carries
ASCII (95 glyphs), the Latin-1 accented letters (95) and Latin Extended-A (127). English needs
the first block and a handful of symbols: 53 KB of the fonts' 188 KB of glyphs. The other two
blocks are 134 KB (F3).

## Findings
What the audits found. "Measured" means a build, the map or the generated data proved the
number; the others are estimates until their task measures them.

| # | Finding | Saves | Fix | Phase |
|---|---|---:|---|---|
| F1 | ESP-IDF's own check and assertion messages: the file, line, expression and text of every `assert` and `ESP_RETURN_ON_ERROR` inside ESP-IDF and lwIP (lwIP 22 KB, the DMA driver 13, chip support 10, FreeRTOS 9, then the drivers) | 182 KB, measured (175 KB of flash, 14 KB of internal RAM; the 7 KB error-name table kept) | `COMPILER_OPTIMIZATION_ASSERTIONS_SILENT`, `COMPILER_OPTIMIZATION_CHECKS_SILENT`. A failure still stops at the same place and the crash report still prints its address, which the build's ELF turns into a line | O1, BL-123 decides which builds |
| F2 | The 42 px headline font is stored plain: 110 KB of bitmaps for text that changes with the panel's state | 64 KB with the clock digits, measured | **Done 2026-09-30:** RLE like the icon fonts (lossless: the preview's 146 pictures identical); the 88 px clock digits too | O2 |
| F3 | Every text font carries the letters of other languages: Latin Extended-A (Polish, Czech, Turkish, Hungarian) and the Latin-1 accented letters (Spanish, French, German) | 134 KB, measured 2026-10-02: Latin Extended-A 79, Latin-1 55 (ASCII and the symbols, 54, stay) | **Done 2026-10-10:** the image's fonts carry English and the shipped translations' letters; both blocks are the letters pack (94,208 bytes), installed by the Flasher, hung behind the built-in fonts as their fallback, as a language pack's fonts are (both blocks: decided 2026-10-10) | O3 |
| F4 | The screen-saver photo is in the image | 126 KB, measured | **Done 2026-10-10:** the picture pack (131,072 bytes, the JPEG 126,093); without it the saver keeps its plain dark background, as it already does when memory is short | O3 |
| F5 | The full root certificate list (145 authorities) | 51 KB, measured (68 full, 17 common) | **Done 2026-10-10:** Espressif's common list built in (15,905 bytes), the full list as the certificates pack (73,728 bytes, a 66,984-byte bundle) made for each version and handed to TLS at start; MQTT and notifications already take their own authority | O4 |
| F6 | `icons_20` (122 icons), `icons_28` (94) and `icons_40` (91) carry nearly the same set, though most icons are drawn at one size: 64 KB for the three | 31.7 KB of bitmaps measured (85.0 -> 53.3), plus the glyph tables | Each size gets the icons drawn at that size, found from the layouts and the code's icon tables, with a preview check that every drawn icon has its glyph | O2 |
| F7 | The Panel Manager's files are gzipped with their comments and indentation: 124 KB, of it `panel-pages.js` 81 | 17.8 KB measured after gzip (123.9 -> 106.1 KB) | Minify before gzip; the page tests run against what is shipped | O2 |
| F8 | The Panel Manager's files could live in the pack store | what F7 leaves, about 85-105 KB | A web pack; the pages must match the firmware exactly, so it comes last | O5 |
| F9 | Every function saves and restores its registers inline | 49 KB, measured (45 KB of flash, 3.8 KB of internal RAM back; 54 KB with LVGL too) | **Done 2026-09-30:** `COMPILER_SAVE_RESTORE_LIBCALLS` (`-msave-restore`, supported by ESP-IDF) for everything but LVGL, whose drawing keeps its inline code (`alarm_optimize_hot_paths()`); calls elsewhere get slightly slower, checked with `display watch` | O1 |
| F10 | The generated screen code (60 KB of code) writes one call per property per object: 912 style calls and 142 `lv_obj_set_name` calls; `screen_main_create` alone is 14.1 KB, `alarm_ui_init_gen` 5.8 | about 21 % of the generated code measured on the host (-21.5 % .text); RISC-V in the image's total below | `lvpro_gen.py` emits shared constant styles where objects repeat a set of properties, names only for the objects the code looks up, and a table walked by one function where a run of calls repeats | O6 |
| F11 | Console commands looked linked into release images, which have no console; checked on the release map: `--gc-sections` discards every console section (ours and ESP-IDF's) once nothing calls `panel_console_start` and the `*_console_register` functions, all inside `#if CONFIG_ALARM_SERVICE_CONSOLE` in `panel_app.c`. The default and dev profiles keep the console on purpose (36 KB: ours 22.8, ESP-IDF's 13.4) | 0: nothing to do (measured 2026-10-02) | None | O6 |
| F12 | LVGL: Montserrat 14 (13.6 KB) is the default font, and the keyboard may draw its symbols with it; three blend targets the 16-bit screen probably never draws into (RGB888 6.6 KB, ARGB8888 13.7, ARGB8888 premultiplied 13.6); the default theme (8.0 KB) is linked though the screens use the simple one | 42.5 KB measured (LVGL 275.3 -> 232.8 KB) | Check the keyboard, the layers (opacity, the saver's picture, the QR code) and what creates the display, then switch them off; the PC preview's LVGL settings stay in step | O1 |
| F13 | The biggest hand-written builders: the Z-Wave screens (`ui_zwave.c`, 46 KB of code and 19 KB of text; `build_device` 7.7 KB, `build_add` 5.7), `build_page` in `ui_settings.c` (9.2 KB), `ui_installer_build` (5.9) | an estimated 5-15 KB, to measure | Look for rows built the same way again and again and give them a table and one loop. Z-Wave stays | O6 |
| F14 | 204 of our functions are dropped by the linker in every profile (14.5 KB; 136 of them in `zwave_proto`) | none in the image | Each is a leftover (delete) or used only by the host tests (keep, or move into the tests). Leftovers still wired in (commands, routes, settings no screen sets) do cost space | O7, later |
| F15 | MQTT over WebSocket is compiled in (`transport_ws.c` 7.6 KB and its part of the MQTT client), though the panel's MQTT settings offer `mqtt` and `mqtts` only | about 9 KB, to measure | Confirm no setting, page or document offers it, then `MQTT_TRANSPORT_WEBSOCKET` off | O1 |
| F16 | lwIP's DHCP server (3.9 KB): the panel never runs an access point | about 4 KB, to measure | Confirm the Wi-Fi link's host code does not need it, then `LWIP_DHCPS` off | O1 |
| F17 | The TLS library is built with options no feature uses: the Brainpool and secp256k1 curves, finite-field Diffie-Hellman (3.9 KB), the CFB, OFB and XTS cipher modes, static-RSA key exchange, renegotiation, CRL and CSR parsing, PKCS#7, the self-tests | measured 6.4 KB (Crypto and TLS 202.2 -> 195.8 KB): the four curves 2.0, CFB/OFB/XTS 2.5, renegotiation, CRL, CSR, PKCS#7 and self-tests 2.0 (with CSR writing); Diffie-Hellman and static RSA are already gone from ESP-IDF 6.1's Mbed TLS 4, so nothing to save; the image stays 2,756,608 bytes (64 KiB steps) | List what each feature needs (TLS 1.2 as client and server, the panel's own certificate, the update signature, Z-Wave S2's Curve25519, CCM and CMAC), switch off the rest one at a time, measure each | O1 |
| F18 | ESP-IDF's and the vendors' components log their own info lines (the Wi-Fi link's alone carry 10 KB of text) at the level our code logs at | an estimated 20-40 KB, to measure | A per-component log level (warning) for ESP-IDF's and the vendors' components; every line of ours stays. An open question once measured: the start-up log loses ESP-IDF's info lines | O1 |
| F19 | ESP-IDF's sleep and power-down code (`sleep_retention`, `regdma_link`, `sleep_modes`: 16 KB) in a panel that never sleeps | up to 16 KB if a setting exists | Look for the setting that leaves it out; record it under "not worth it" if there is none | O1 |

Looked at and not worth it: networking, TLS, the C6 link and ESP-IDF's drivers are many small
pieces, all in use; turning IPv6 off saves about 15 KB and loses IPv6. The RISC-V ZCMP
instructions do nothing on v1 silicon (ESP-IDF adds them for v3 only) and have hardware errata.
Compressing the small text fonts
as F2 did the big ones would save about 10 KB of what F3 leaves and slow every label. Loading
the screens from XML at run time would move 68 KB out and bring a parser of about that size in.
ESP-IDF's binary log format would take the log text out of the image, ours too (about 85 KB),
but then every log line needs the build's ELF to be read, which OPT-008 rules out.

Held back until more space is needed (decided 2026-10-10; measured by the size study
of that day):
- **The Wi-Fi link's message tables, about 43 KB.** ESP-Hosted's generated `gen_v2.c` (115.5 KB:
  69 KB of descriptors, 38.9 KB of message and field names) carries every name, and the panel
  reads none. Generated again from a copy of its `rpc_v2.proto` with `optimize_for = CODE_SIZE`,
  protoc-c leaves the names and the by-name indexes out; the bytes sent to the C6 stay the same
  and its firmware is untouched. It needs protoc-c 1.5.2 (the version that made ESP-Hosted's
  file), a build check that fails when ESP-Hosted's proto changes, and Wi-Fi tried on a panel:
  joining, a scan, a reconnect, an update.
- **A speech-only voice, about 56 KB.** The voice decoder's music mode (Opus CELT, 58 KB of code)
  serves 94 of voice pack r8's 7,641 packets; the rest are speech mode (SILK). A pack encoded in
  speech mode only lets the decoder leave the music mode out: micro-opus patched, the pack tool
  made to encode SILK only (a new pack, listened to again), the reader refusing other packets.
  The firmware and the new pack arrive together, or the panel has no voice.
- **The web pack (O5).**

### What it adds up to
From 3,084 KB (65.4 %), in the order of the phases; the image moves in 64 KiB steps, so the
percentages are where the sum lands, not promises.

| After | Saves | Image | With Zigbee |
|---|---:|---:|---:|
| O1: build settings (F1 175, F12 up to 55, F15-F19 an estimated 40-70) | ~260 KB | ~59.9 % | ~65.5 % |
| O2: icons and minified pages (F6, F7) | ~50 KB | ~58.8 % | ~64.4 % |
| O3: the letters and the photo as packs (F3 134, F4 126) | 260 KB | ~53.3 % | ~58.9 % |
| O4: the certificate list as a pack (F5) | 51 KB | ~52.2 % | ~57.8 % |
| O6: our code (F10, F13; F11 in release builds only) | ~35 KB | ~51.5 % | ~57.1 % |
| O5: the Panel Manager's files as a pack (F8) | ~85 KB | ~49.7 % | ~55.3 % |

Measured on 2026-10-02 after O0-O2 and F10: 3,084,288 -> 2,756,608 bytes, 65.4 % -> 58.4 % (five
64 KiB steps; F1+F15+F16 196 KB, F12 42.5, F6 31.7 of bitmaps, F7 17.8, F10 about a fifth of the
generated code, F17 6.4). Measured 2026-10-10 like for like (version 0.7.0, e131a1d against ab2bfa0): release 3,477,504 -> 3,149,824 bytes (73.7 % -> 66.8 %), dev 3,739,648 -> 3,477,504 (79.3 % -> 73.7 %): 5 and 4 64 KiB steps, 327,680 and 262,144 bytes, for the
packs F3, F4 and F5 (311 KB together).

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| OPT-001 | Every feature the firmware had on 2026-09-30 stays in every P4 image, Z-Wave included | review; the preview walks; CI's firmware build |
| OPT-002 | The v1 default image fills less than 60 % of its OTA slot once O1-O3 are done | `tools/check_image_size.py` (CI) |
| OPT-003 | Every firmware build can report its size by category, with our biggest symbols, so growth is seen where it starts | `tools/size_report.py`, its tests, CI |
| OPT-004 | Drawing is no slower: `display watch 60` while someone uses the screen shows no worse times than before | bench |
| OPT-005 | Data stored differently draws the same pictures (lossless) | the preview's pictures unchanged |
| OPT-006 | Data moved into the pack store follows ADR-045: signed with the firmware key, checked at install and at every load, strictly validated, and the panel works without it, with a stated fallback for each kind | host tests, Flasher tests |
| OPT-007 | A missing asset pack is installed by the Flasher and fetched by the panel the way a language pack is | Flasher tests, host tests, bench |
| OPT-008 | Diagnostics stay usable: a crash report still locates the failure (its address and the build's ELF) and our own log messages are unchanged | bench |
| OPT-009 | Code is deleted only when nothing uses it: not the firmware, not the host tests, not an older stored format the firmware must still read | review, host tests |
| OPT-010 | The image's text fonts carry English: ASCII, the punctuation and symbols the English screens use, and the letters of the translations the project ships (`i18n_chars.txt`). Every other letter comes from the letters pack or a language pack's fonts, and a letter no loaded font has draws as the placeholder box, never as nothing (I18N-011) | the preview with and without the letters pack; host tests |
| OPT-011 | A build setting is switched off only after the code shows that no feature uses what it removes, and the tests of the features beside it pass | review; host tests; the firmware build |
| OPT-012 | An asset pack is one file format (`RAST` format 1, [asset-packs.md](asset-packs.md)) read by one C reader and its Python twin, checked against one frozen fixture; every malformed file is refused before anything uses it | `test_asset_pack.c`, `tools/tests/test_asset_pack.py` |
| OPT-013 | The certificates pack is used only when it was made for the running firmware, and is loaded before any TLS connection; otherwise the panel trusts the common list built into the image | host tests; bench |
| OPT-014 | A text font's fallbacks are, in order, the language's font for its role (when the language brings one) and then the letters font | the preview with and without the letters pack and a language font |

## Design
Six levers, cheapest first. Each change is measured before and after with the size report, and
its saving written into the Findings table.

1. **Size report (O0).** The audit's scripts become `tools/size_report.py <build>`: the table
   above, our biggest functions and data, the string literals per object, the fonts' bytes per
   block of characters, and a comparison of two builds. CI prints it for the v1 build, so a jump
   like the translations' or the redesign's shows in the change that caused it.
2. **Build settings (O1).** F9 for every build; F1 for the builds BL-123 decides; F12 and
   F15-F19 after their checks. Each one is a setting in `sdkconfig.defaults*` (F18 a compile
   definition per component), so a full rebuild follows and the units run one after another.

   **What the TLS library must keep (F17, OPT-011).** The options off are listed in
   `sdkconfig.defaults`; anything a new feature needs from Mbed TLS is added here first. The P4's
   Wi-Fi security runs on the C6, so no Wi-Fi code uses this library.
   - TLS 1.2 clients (`panel_web` fetches, `net_mqtt`, `net_notify`, `panel_ota` and `lang_packs`
     downloads): ECDHE-ECDSA and ECDHE-RSA with AES-GCM and AES-CBC, SHA-1/256/384/512, the
     certificate bundle's RSA (PKCS#1 v1.5 and PSS) and P-256/384/521 authorities (the image
     holds the common list, `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN`; the full list comes
     as the certificates pack, asset-packs.md), SNI, ALPN.
   - TLS 1.2 servers (`panel_web` HTTPS, `ha_link` with client certificates): the panel's own
     ECDSA P-256 key, ECDHE, X.509 parsing, the peer's certificate kept for pinning.
   - The panel's own certificates (`dev_cert.c`, `ha_cert.c`): ECDSA P-256 key generation and
     signing (deterministic), SHA-256, X.509 certificate writing (not CSR writing), PEM.
   - Update and pack signatures (`panel_ota`, `lang_packs`): RSA-3072 PSS with SHA-256 (ESP-IDF's
     signed-app check, docs/features/13).
   - Backups (`backup_psa.c`): PBKDF2-HMAC-SHA256 and AES-256-GCM.
   - Codes (`code_hash.c`): SHA-256 (it builds HMAC itself).
   - Z-Wave S2 (`zw_psa.c`): one-block AES-128 (CTR, CMAC and CCM are built on it in `core/zwave`)
     and X25519 (Curve25519).
3. **Stored better inside the image (O2).** RLE for the big fonts drawn when the state changes
   (headline, clock digits): lossless, a few dozen glyphs unpacked per redraw, so OPT-004 is
   checked during an alarm, when the state ring's glow redraws the hero. Each icon size carries
   only its icons. The Panel Manager's files minified before gzip.
4. **Asset packs (O3-O5, ADR-046).** The pack store keeps whole signed blobs by kind;
   languages are the first kind. Fonts (the letters English does not use, for the five text
   roles, hung behind the built-in fonts as their fallback, as a language pack's fonts are), the
   saver picture, the full certificate list and later the Panel Manager's files become kinds of
   their own. Each is signed with the firmware key and checked at install and at every load. Each
   has a fallback when missing: a missing letter draws as LVGL's placeholder box, the saver keeps
   its plain background, TLS uses the built-in common list, the Panel Manager shows a built-in
   page that fetches its files. The Flasher installs the default set with the firmware, and the
   panel fetches a missing or outdated one from the release manifest, as it does a language.
5. **Our code (O6).** The generator's output (F10), console code in release images (F11) and
   the biggest builders (F13), each measured first.
6. **Leftovers (O7, later).** The linker's discarded list of our functions in every profile,
   cross-checked against the host tests; then what is still wired in but unused: console
   commands, API routes, settings no screen sets, compatibility code for formats no panel holds
   any more (never one a stored format's older data still needs, CONTRIBUTING.md).

## Interfaces and data
- Pack kinds in the pack store's table are a stored contract (CONTRIBUTING.md, language packs):
  `1` language, `2` voice; O3 adds `3` letters, `4` picture, `5` certificates, and O5 `6` web,
  with a frozen fixture and the Python twin (`tools/flasher/roboflash/packstore.py`) in the same
  change; never renumber.
- Each asset pack is a `RAST` file: a 128-byte header (kind, name, the firmware version it was
  made with, what the firmware matches) and a table of parts, signed like a language pack
  ([asset-packs.md](asset-packs.md), OPT-012).
- Build settings live in `firmware/crowpanel_p4/sdkconfig.defaults*`.

## Test and acceptance
- Every change: the size report before and after, the saving recorded in Findings.
- Lossless changes (RLE, minifying, icon sets, the generator, LVGL's settings): the preview's
  pictures unchanged; the Panel Manager's tests run against the shipped files.
- A setting switched off (F12, F15-F19): the search that shows nothing uses it named in the
  change, the host tests, a firmware build; for F17 also, on a panel, the Panel Manager's
  sign-in, MQTT over TLS, a notification and an update's download.
- Asset packs: host tests for each kind (good, missing, bad signature, damaged, wrong kind),
  the Flasher's install and update tests, and on a panel: the saver, a name with an accented
  letter with and without the letters pack, TLS to a server with an uncommon authority with and
  without the pack.
- On a panel: `display watch 60` while using the screen and during an alarm (OPT-004), a crash
  report read back to its line (OPT-008).

## Open questions
- **Silent ESP-IDF checks (F1): in which builds?** Release only, or every build but dev,
  so the default build's number drops too? Leaning: every build but dev.
  BL-123.
- **ESP-IDF's own info lines (F18):** once measured, worth losing them from the start-up log?
  Our own lines stay either way.
- **A lower size limit in CI** once O1-O3 are done (70 % instead of 85 %), so growth is noticed
  earlier? Leaning: yes.

## Tasks
Each box is one unit: one piece of work, with the check that proves it. Every unit reports the
size report's numbers before and after, and writes its saving into Findings.

**Order.** O0 first: every later unit measures with it. Then O1 and O2. Units that change
`sdkconfig.defaults*` or the generator's output run one after another in the main tree (each
needs a full firmware build, and the main tree holds the signing key); the minifier (F7) and the
size report touch nothing the others do and can run beside them. O3 starts with the pack kinds;
the letters, the picture and the certificate list follow in any order. Stored formats, trust and
crypto (the pack kinds and header, F5, F17) are for careful work and a review; the rest is
well-specified work. The panel checks are bench work.

### O0 - Size report (BL-122)
- [x] `tools/size_report.py <build>`: categories, our biggest functions and data, strings charged back, the fonts' blocks, `--compare`; 15 tests; CI prints it for the v1 build (OPT-003) (BL-122)

### O1 - Build settings (BL-122)
- [x] `-msave-restore` for all but LVGL's drawing (F9): 45 KB flash, 3.8 KB internal RAM less (BL-122, b4f4320)
- [x] Silent ESP-IDF checks (F1) in every build but dev, as BL-123 leaned: with F15 and F16, 196 KB less, 65.4 % -> 61.2 % (BL-123)
- [ ] On a panel, for F1: a crash report of a non-dev build read back to its line (OPT-008); the decision on BL-123 (BL-123)
- [x] LVGL (F12): the default theme off, UNSCII 8 as the default font (Montserrat 14 off: the keyboard's symbols and the placeholder draw from our fonts), premultiplied ARGB8888 off; RGB888 and plain ARGB8888 kept (the layers use them); the preview's `lv_conf.h` in step, its pictures unchanged: LVGL 275.3 -> 232.8 KB, the image 2,887,680 -> 2,822,144 bytes (BL-122)
- [x] MQTT over WebSocket (F15) and the DHCP server (F16) off: `net_mqtt.c` sets TCP or TLS only, no code starts an access point (OPT-011) (BL-122)
- [x] The TLS library's unused options (F17) off: 13 options in three measured groups (6.4 KB; `sdkconfig.defaults` names them), the list of what every feature needs under Design; two groups (finite-field DH, static RSA) were already gone in Mbed TLS 4; the host tests pass (82/82, through the host crypto shim) (OPT-011) (BL-122)
- [ ] On a panel, for F17: the Panel Manager's sign-in, MQTT over TLS, a notification, an update's download (BL-122)
- [ ] ESP-IDF's and the vendors' info lines (F18) measured with a per-component log level, and ESP-IDF's sleep code (F19) looked into; the numbers recorded, switched on only once the open question is answered (BL-122)

### O2 - Stored better inside the image (BL-122)
- [x] The headline and clock digit fonts RLE-compressed (F2): 64 KB less, the preview unchanged (BL-122, b4f4320)
- [ ] On a panel, for F2 and F9: `display watch 60` while using the screen and during an alarm, no worse than before (OPT-004) (BL-122)
- [x] Each icon size carries only its icons (F6): 20/28/40/96 px hold 121/60/15/22 glyphs (were 122/94/89/23; bitmaps 85.0 -> 53.3 KB), and the preview fails on any drawn icon without a glyph (`report_glyphs` in `host/ui_preview/main.c`, every label and key at every shot); it found the favourites sheet's lightbulb missing at 20 px, now added; every runtime icon table (hero, pad, zone class, troubles, device kinds, wizard) checked against its size by hand (BL-122)
- [x] The Panel Manager's files minified before gzip (F7): `minify_asset.py` (token-aware, no dependency) runs before `gzip_asset.py`; gzip 123.9 -> 106.1 KB; the page tests pass on the source and, with `PANEL_WEB_DIR`, on the minified files (117 + 7 both ways) (BL-122)

### O3 - Asset packs: the letters and the saver photo (BL-124)
- [x] Pack kinds and the asset header, C and Python with frozen fixtures (BL-124, 9d729a8)
- [x] The letters pack (F3): the image's fonts with English and the shipped translations' letters, the pack made by the build, hung behind the text fonts (OPT-010, OPT-014); the preview with and without it (BL-124, c3abc63, 2be59ef, 6a6b192)
- [x] The asset packs installed by the Flasher and fetched by the panel from the update server's lists (OPT-007; the server's side is BL-208) (BL-124, dfc216c, 5a2f29f, ee0cbda, ab2bfa0)
- [x] The keyboard without the letters pack: no àé key until the pack is loaded, the space bar in its place (approved 2026-10-10; docs/features/07 mockup `screens/settings.js`) (BL-124, 6a6b192)
- [x] The picture pack (F4): the saver reads its photo from the pack; seen on a panel 2026-10-10 (BL-124, e353b5a)
- [ ] On a panel: a name with an accented letter with and without the letters pack (the saver: seen 2026-10-10), and an update that brings the packs along once the firmware server lists them (BL-208) (BL-124)

### O4 - The certificate list as a pack (BL-124)
- [x] The common list built in, the full list as a pack handed to TLS at start (F5); on a panel 2026-10-10 the pack loads and TLS takes it (BL-124, 995509c)
- [ ] On a panel, a server whose authority only the full list has, with and without the pack (BL-124)

### O5 - The Panel Manager's files as a pack (BL-124)
- [ ] Held back until more space is needed (decided 2026-10-10). Then, once O3's packs work on a panel: a drawn proposal of its built-in fallback page (WEB-026), then the web pack (F8) (BL-124)

### O6 - Our code (BL-125)
- [x] The generator's output measured and made compact (F10): `lvpro_gen.py` emits shared run functions (`alarm_ui_runs_gen.c` per layout), names only for objects the code looks up, style tables; host .text of the generated code -21.5 %; the pictures identical (207 + 269), `check_ui_contract.py` passes; a review found six can-wait items (BL-125's row) (BL-125)
- [x] Console commands only in builds with a console (F11): already so; the release map places no console section, the registrations sit inside `#if CONFIG_ALARM_SERVICE_CONSOLE` (BL-125)
- [ ] The biggest builders reviewed for repetition (F13): the Z-Wave screens, `build_page`, `ui_installer_build`; the preview's pictures unchanged (BL-125)
- [ ] The generator review's can-wait items (F10): the code-file list the name heuristic scans shared between `lvpro_gen.py` and `check_ui_contract.py`, with a test; the `opa` and selector parameter types checked (BL-125)

### O7 - Leftovers (later; BL-126)
- [ ] The functions the linker drops in every profile sorted: used only by the host tests (keep, or move into the tests) or left over (delete) (F14) (BL-126)
- [ ] What is still wired in but no longer used: console commands, API routes, settings no screen sets, compatibility code no stored data needs (BL-126)
