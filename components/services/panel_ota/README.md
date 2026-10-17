# panel_ota

**Tier:** services (ESP-IDF, board-agnostic) · **Features:** [13 OTA updates](../../../docs/features/13-OTA-Updates/README.md), [12 C6 co-processor firmware](../../../docs/features/12-C6-Coprocessor-Firmware/README.md) (the update path) ·
**Tests:** the portable helpers are host-tested in [test_fw.c](../../core/alarm_proto/test/test_fw.c) (`alarm_proto`'s versions, release manifest, install policy, serial framing and update entity, plus the retry schedule and signed-profile rule this component uses, `alarm_proto/ota_retry.h`
and `alarm_proto/ota_profile.h` since docs/features/33 step 8; `OTA-002`, `-004`, `-010`, `-013`, `-014`, `-015`; for updates through the panel link the header judge, the cap, the why numbers and the image walk, `LINK-029`, `-031`, `-033`, `-037`), and [test_fwsrv.c](../../core/alarm_proto/test/test_fwsrv.c) (a check with `want` and its why). On a panel: the `ota` and `c6` console commands, `tools/ota_serial.py`, `tools/panelctl.py flash`, and the owner-authorised scripts in [tools/tests](../../../tools/tests) (`hardware_remote.py` rollback, `hardware_tamper.py`, `panel_web_ota_hil.py`).

Firmware updates for the panel and its Wi-Fi co-processor. One writer takes an image from any
transport (a release download, the serial console upload, `dev_remote`, `panel_web`) into the
other OTA slot. It checks project name, chip and version as soon as the image header arrives,
and length and SHA-256 at the end; `esp_ota_end()` rejects anything not signed with the running
image's key (RSA-3072 signed apps, no eFuse Secure Boot) or with the one key the installer accepted
for the update server: `ota_trust.c` wraps ESP-IDF's check (`-Wl,--wrap`), lets its "yes" stand
and tries the accepted key only after its "no", failing closed (OTA-020). A verified image is started only
when the system is idle, asked of the alarm task itself and asked again just before the boot
slot is switched; after the switch the alarm task makes the last check (the engine idle, nothing
waiting in its queue) and restarts the panel itself, or the slot is switched back and the restart
put off (`alarm_service_waiting()`). The first boot of an update is on
probation: every part in the start-up mask must keep sending heartbeats for at least 60 s (at
most 180 s) or the image marks itself invalid and the previous one returns (the rule and its
times: `alarm_proto/ota_selftest.h`, host-tested). After a change of
server the previous version may be signed with a key the new one no longer accepts; ESP-IDF's
rollback check then refuses it, and the image restarts unconfirmed instead, which the bootloader
(no signature check at boot) turns into the same switch back. Language packs signed with the old
key read as damaged after the change until the new server's packs are fetched. The update task also
asks the update server (a domain, `POST /api/v1/check`, OTA-017/018) on a schedule and only
reports what is available; installing always
needs a person. For a language added later it also asks for the running version's packs
(`GET /api/v1/languages`, `panel_ota_running_languages_ask()`). Every answer is read in
small pieces with a 1 s timeout, so a stop ends a download within seconds, and a body slower than
1 KB/s over a minute is given up (OTA-027); a redirect's body is never read. The co-processor image
in `cp_fw` is streamed to the C6 over ESP-Hosted (`cp_ota.c`); the C6 keeps its old image until the
panel restarts, and that restart waits for an idle system and goes through the same announcement
and last gate on the alarm task as a release's (OTA-004, OTA-028), keeping the update machinery until then. Every
step is an event in the log and on MQTT (OTA-010).

Updates through the panel link (docs/features/11 "Updates through the link") are built in but run
only when a caller asks, so a stand-alone panel and a master without keypads update as before. On a
keypad: its master's orders (`panel_ota_order_probe/fetch/relay/install/cancel`, the outcome and
its why in `panel_ota_order_get()`), PROBE and FETCH as checks with `want` at the order's server
and channel for that run only (a PROBE with no server it can use is answered NO_LOOKUP 2; only a
FETCH without one is refused, BAD_ORDER 40), RELAY as a run of `PANEL_OTA_KIND_LINK` through the
writer (no server needed), the restart held for the master's INSTALL, no scheduled check
(`panel_ota_set_checks`) and never past its master (`panel_ota_set_version_cap`; an ordered run is
capped at its order's version, its master's target, instead). The order's server and key become
its trust record when they differ (in RAM at once, stored by the update task; LINK-036, nobody is
asked). After an install its master ordered, "ordered install of V" (NVS `ord`, with the attempt)
puts its self-test on trial with its master (`panel_ota_set_trial_check`, `panel_ota_ordered_trial`):
WAIT holds the confirmation, FAIL rolls back. On a master: its own restart put off while a keypad
installs or self-tests (`panel_ota_set_restart_hold`, which wakes the update task only when the
hold changes) or as its keypads' hook says (`panel_ota_set_restart_with`: `hold` at every gate,
then `installs_out` asked for at most 5 s once the gate passed, `panel_ota_restart_soon()` true
meanwhile; from then on no keypad's hold counts), its keypads in its self-test
(`panel_ota_set_keypads_check`, `PANEL_OTA_OK_KEYPADS`: until the self-test's deadline no keypad
back yet is waited for, never a failure, and 10 s of a keypad's answers prove it), and its image
sources for the relay (`ota_source.c`: the running image's size, digest and facts once confirmed,
read through 256 KB windows; its own release waiting in its spare slot, `panel_ota_spare_image`
and `panel_ota_spare_read`, its digest kept as it was written; one copy in PSRAM fetched with a
keypad's facts, `panel_ota_fetch_for` - also while its own release waits - freed the moment the
master's own run starts). `panel_ota_device()` and `panel_ota_get_facts()` give a keypad's FW_INFO.

## Public API
| Header | What it offers |
|---|---|
| `include/panel_ota.h` | `panel_ota_start(required)`, the heartbeat contract (`panel_ota_healthy()`, `panel_ota_health_stale()`, `PANEL_OTA_OK_*` bits), status and listeners, the update server (check, commit, channel, facts, wanted locales, the running version's languages) / check / install / stop / cancel / discard, the writer (`panel_ota_begin/write/end/abort/apply`), `panel_ota_system_idle()` and the gate that answers it on a panel with no engine of its own (`panel_ota_set_idle_gate()`, a link keypad, docs/features/11), remote-development probation (`panel_ota_remote_prepare/confirm/operation`, `panel_ota_cancel_remote()`), `panel_ota_cp_update()`, `panel_ota_console_register()` |
| (in `alarm_proto`) `alarm_proto/ota_profile.h`, `alarm_proto/ota_retry.h` | which signed project names may replace the running one; the retry schedule after a failed check (moved there in docs/features/33 step 8 so they are host-tested as part of a core component) |
| `include/panel_ota.h`, updates through the panel link | a keypad's side: `panel_ota_set_checks()`, `panel_ota_set_version_cap()`, `panel_ota_order_*()`, `panel_ota_key_trusted()`, `PANEL_OTA_KIND_LINK`, `panel_ota_set_trial_check()`, `panel_ota_ordered_trial()`, `panel_ota_get_facts()`; a master's: `panel_ota_set_restart_hold()`, `panel_ota_set_restart_with()`, `panel_ota_restart_soon()`, `panel_ota_set_keypads_check()` (`PANEL_OTA_OK_KEYPADS`, `last.failed`), `panel_ota_device()`, `panel_ota_running_image()`, `panel_ota_running_read()`, `panel_ota_spare_image()`, `panel_ota_spare_read()`, `panel_ota_fetch_for()`, `panel_ota_copy_get/read/free()` |
| (in `alarm_proto`) `alarm_proto/proto_fw.h` (`src/fw_image.c`) | the why numbers 20-61, the cap, the header judge and the image walk those use |
| `panel_ota_internal.h` (private) | shared between `panel_ota.c`, `cp_ota.c`, `ota_console.c`, `ota_trust.c` and `ota_source.c` |

## Depends on
- Project components: `alarm_core` (public: `alarm_types.h`), `platform_esp` (`alarm_service` for the idle gate and firmware events, `panel_log` flush), `alarm_proto` (`proto_fw`: versions, install policy; `proto_fwsrv`: the server's domain, check, keys and trust record), `alarm_mem` (slow memory).
- ESP-IDF: `esp_common` (public), `app_update`, `bootloader_support` (`verify_rsa_signature_block`), `console`, `esp_app_format`, `esp_driver_uart`, `esp_http_client`, `esp-tls` (a lookup that failed, for the link's why), `esp_partition`, `mbedtls`, `nvs_flash`; `espressif/esp_hosted` `~3.0.9` on the ESP32-P4 (`idf_component.yml`).

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `PANEL_OTA_DEVICE` | empty: the board's id (`board_info()->id`, passed in with `panel_ota_set_device()`); `crowpanel_dhe04107d` in `firmware/crowpanel_p4` | The `device` a check sends: the product name the server's catalogue uses |
| `PANEL_OTA_DEFAULT_DOMAIN` | `roboalarms.com` | This build's update server; used until the installer keeps another (`ota server check <domain>`, `ota server use`) and when the stored one is damaged; empty: no automatic checks |
| `PANEL_OTA_CHECK_HOURS` | 24 (0-720) | Automatic check interval; 0 = only on request |
| `PANEL_OTA_CP_AUTO` | y | Send the `cp_fw` image to the co-processor once idle when it differs from what runs; a failure is not retried automatically |

The co-processor update needs `CONFIG_ESP_HOSTED_HOST` (and its OTA feature); `cp_ota.c`
compiles without it. The signing and rollback options in `firmware/crowpanel_p4/sdkconfig.defaults`
are what make the checks above real.

## Resources
- Task `ota`: 8192-byte internal stack, priority 3 (`panel_ota.c:1385`); it writes the image and NVS. The serial upload runs on the console's task.
- Heap per check or download: an 8192-byte answer (4096 for a server's keys), a 2048-byte request and a 4096-byte HTTP buffer; one HTTPS client session, verified with the certificate bundle (`:871`). Slow memory (`alarm_mem`): the idle-gate snapshot (`:192`).
- Partitions (`firmware/crowpanel_p4/partitions.csv`): `ota_0` / `ota_1` (4608K each), `otadata`, `cp_fw` (1536K, read by `cp_ota.c:118`).
- NVS namespaces `ota` (`trust`: the server and its accepted key, a keypad's taken from its master's orders too; `chan`: the channel; rollback note, co-processor attempt; `rbh`: what a failed self-test missed, written only on a master that asked its keypads; `ord`: a keypad's "ordered install of V", `ota_rec_t` written with the attempt, erased once confirmed or back; the old `src` is no longer read) and `devota` (the remote probation record, `panel_ota.c:689`).
- Updates through the panel link, only when used: the order and the probe's answer in slow memory (under 1 KB); on a master, a mutex made on the first use, one 256 KB window of the running partition mapped while the relay reads it (unmapped after 30 s unused), and one copy for a keypad in PSRAM (at most `PANEL_OTA_COPY_MAX`, 4,010,803 B; freed after 30 min unused or when the master's own run starts). Mapping a window disables the cache for a moment on the P4: `panel_ota_running_read()` only from a task with an internal-RAM stack (the link task).

## Rules
- Never replace or commit `secrets/ota_signing_key.pem`: the panel accepts only images signed with it. Keep the signing and rollback options in `sdkconfig.defaults` (CONTRIBUTING.md).
- Never run a C6 co-processor update, or enable eFuse Secure Boot or flash encryption, unless the owner asks explicitly (AGENTS.md, "Working with a real panel").
- `panel_ota_health_t` bits: append, never renumber. Heartbeats come from the task that does the work, at least every 5 s (`panel_ota.h`).
- `panel_ota_system_idle()` waits for the alarm task: call it from the update or console task, never from a listener (`panel_ota_internal.h`).
- Changing `partitions.csv` means a full erase of the panel: avoid it (CONTRIBUTING.md).
- Planned (docs/features/33 step 8): the C6 part only with ESP-Hosted, the device name from the project, the partition labels from the table.
