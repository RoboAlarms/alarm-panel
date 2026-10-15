# lang_packs

**Tier:** services (ESP-IDF, board-agnostic) · **Features:**
[37 Globalization](../../../docs/features/37-Globalization/README.md), design in
[language-packs.md](../../../docs/features/37-Globalization/language-packs.md) · **Tests:** the
store and the pack reader are host-tested ([pack_store](../../core/pack_store/test),
[i18n](../../core/i18n/test)), including the install transaction pack_assets runs
(`ps_put_blob`, AUD-027); this service is checked on a panel (BL-121)

**pack_assets** (`include/pack_assets.h`, `src/pack_assets.c`; docs/features/08
`voice-first-build.md` "Installing a voice pack", AUD-027) is the one owner of the `assets` store
for language packs (kind 1) and voice packs (kind 2): the only `ps_store_t`, the partition, one
recursive mutex, the signature check and the update machinery's claim. Built when
`ALARM_FEATURE_LANG_PACKS` or `ALARM_FEATURE_VOICE` is on; `pa_init()` mounts once, for whichever
starts first. `pa_install()` is one transaction for any kind: size bounds (language 8 KiB-2 MiB,
voice 8192-970752 bytes, letters 256 KiB, picture 512 KiB, certificates 128 KiB), the claim, the signature, the envelope (`ps_envelope`), the kind's
validate callback, then `ps_put_blob` under the mutex (admitted only when a free run holds the
whole new copy beside the working one; gate; erase, write, read back and compare; gate; commit),
then the kind's done callback. Nothing else mounts or writes the partition.

Every language but English is a signed pack file in the `assets` partition (a pack store,
`components/core/pack_store`, owned by pack_assets). This service keeps a header cache in RAM, and loads
a pack the panel speaks into PSRAM: copied through the flash mapping, its SHA-256 checked against the
signature sector with the key the running firmware was signed with
(`esp_secure_boot_verify_sbv2_signature_block`), parsed by `i18n_pack_open()` and registered in an
append-only table that readers scan without a lock. A registered pack is never freed.

Flash work (the partition, the mapping, the signature check) runs only on the start-up task and
this service's own task, both with internal-RAM stacks. The readers touch RAM only. A failure never
stops the panel: it speaks English, entry by entry where a pack lacks one (I18N-042). Start-up adopts
the store's default language when none is stored, then loads the stored language's pack.

**Install and remove** (`lang_packs_install()`, `lang_packs_remove()`) refuse while a partition is
armed or an alarm sounds. An install checks the size, the signature, the payload and the draft and
English rules, writes the blob into free sectors, reads it back and compares it, and only then
commits the table (a failed commit is checked with `ps_find`: it may have landed) - all through
`pa_install()`, whose mutex is also this service's load mutex. If the panel speaks the locale the new pack is loaded at once and
a replaced one is hidden. The caller needs an internal-RAM stack (flash writes switch the cache off).
The pack it replaces stays until the new one is in, so a replacement needs room for both.

**Downloads** (`lang_packs_fetch()`, I18N-047): `panel_ota` keeps the release manifest's `languages`
at each check; the service task downloads a listed pack (`panel_ota_download()`: redirects, the
certificate bundle, size and SHA-256 checked) and installs it. It also does so by itself after a
release check (a `panel_ota` listener wakes it), once per language per check, never while an update
downloads or the panel is locked: for the panel's language when no usable pack is installed, and for an
installed pack made for another firmware version when the list has one for the running version.
A language the last check didn't list at the running version (a check lists the release it offers)
is looked up first in the server's list for the running version (`GET /api/v1/languages`,
through `panel_ota_running_languages_ask()` on the update task), then in the check's list;
when neither has it the next check asks for it too, as before. Its own signature is the trust anchor
either way (OTA-016).
`lang_packs_fetch_status()` shows the last download; a download that fails reports `no_source`.

**Asset packs** (`include/asset_packs.h`, `src/asset_packs.c`; docs/features/39 asset-packs.md
"The panel", OPT-006): the letters (kind 3, `letters`, at most 256 KB), the saver's picture (kind
4, `saver`, 512 KB) and the certificates (kind 5, `certs`, 128 KB), from the same store.
`asset_packs_start()` (the start-up task, after `lang_packs_start()`, before Wi-Fi) loads each
kind's installed pack: copied into a 4-aligned PSRAM buffer, its signature checked (`pa_verify`),
opened with `asset_pack_open()` and held to the kind's rules - the header's kind and name are the
entry's, the letters' parts are fonts, the picture is one `photo` baseline JPEG of at most
1024 x 600, the certificates one `bundle`, made for the running version (else refused: the image's
own list), a draft refused on a release firmware. A kind's state is `none`, `ready`, `refused`,
`damaged`, `needs_restart` or `unchecked` (no memory, a flash read, an update holding the check).
The screen reads the offered copies lock-free (`asset_packs_font()`, `_picture()`,
`_letters_match()`, `_generation()`). At the end of its start `asset_packs_start()` hands the
loaded certificates bundle to `esp_crt_bundle_set()` (OPT-013), before anything opens TLS; a
bundle that fails that check is `refused` and TLS keeps the image's common list.
`asset_packs_install()`
(the dev link's `PUT /dev/v1/asset`) refuses while armed or in an alarm and runs `pa_install()`;
the letters and the picture load at once, the certificates at the next start. A replaced copy is
never freed: a kind loads at most four copies between restarts. Without
`ALARM_FEATURE_LANG_PACKS` every call answers "none".

**Console** (`lang`, `asset`): the packs, the update source's list and the last download; `lang load <locale>`,
`lang rm <locale>`, `lang fetch <locale>`, `lang verify` (every pack's signature from flash). Load, remove
and verify run on the service task. `asset` lists each asset kind's state, version, match and bytes and
the store's free bytes; `asset verify` checks each installed asset pack again from flash (signature,
format, the kind's rules), on the same task.

Memory: the tables (header cache, loaded packs, bad list) and the pack store's buffer are in PSRAM
(`EXT_RAM_BSS_ATTR`, docs/bugs/B001); only the mutexes and small scalars are internal.

## Public API
| Header | What it offers |
|---|---|
| `include/pack_assets.h` | `pa_init()`, `pa_ready()`, `pa_lock()` / `pa_unlock()`, `pa_find()`, `pa_count()` / `pa_entry_at()`, `pa_read()`, `pa_view()`, `pa_verify()`, `pa_install()` (`pa_install_t`), `pa_remove()`, `pa_free_bytes()`, `pa_generation()`, `pa_result_t` / `pa_result_name()` |
| `include/asset_packs.h` | `asset_packs_start()`, `asset_packs_list()` (`asset_info_t`, `asset_state_name()`, `asset_kind_name()`), `asset_packs_free_bytes()`, `asset_packs_generation()`, `asset_packs_font()`, `asset_packs_picture()`, `asset_packs_letters_match()`, `asset_packs_certs()` (RAM only); `asset_packs_install()` (`asset_result_t` / `asset_result_name()`), `asset_packs_verify_all()` |
| `include/lang_packs.h` | `lang_packs_start()`, `lang_packs_catalog()`, `lang_packs_list()`, `lang_packs_generation()`, `lang_packs_known()`, `lang_packs_font()` (a loaded pack's font for a role, RAM only); `lang_packs_install()`, `lang_packs_remove()`, `lang_packs_fetch()`, `lang_packs_fetch_status()`, `lang_packs_free_bytes()`, `lang_packs_available()`, `lang_result_t` / `lang_result_name()`; `lang_packs_console_register()` |

## Depends on
- Project components: public `i18n`, `pack_store` (pack_assets.h names `ps_entry_t`); private `platform_esp` (the language preference, the alarm snapshot), `panel_ota` (the manifest's languages, the download), `alarm_core`, `alarm_proto`
- ESP-IDF: private `esp_partition`, `esp_app_format`, `console`, `heap`, `log`, `freertos`, `mbedtls` (PSA SHA-256), `bootloader_support`, `esp_rom`

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `ALARM_FEATURE_LANG_PACKS` | on with PSRAM | off: stubs, English and the built-in catalogs only |
| `ALARM_I18N_BUILTIN` | off with packs | off: `I18N_NO_BUILTIN` for `components/core/i18n`, the image holds English only |

A firmware signed without Secure Boot V2 or signed apps refuses every pack. The firmware build
makes the packs (`tools/i18n.py pack`, `cmake/alarm_firmware.cmake`) into `<build>/lang/`, and the
asset packs (`tools/asset_pack.py`: `letters.roboasset` from the layout's `fonts/letters/`,
`saver.roboasset` from the saver's photo; drafts unless a release) into `<build>/assets/`.
