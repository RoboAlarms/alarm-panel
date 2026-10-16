# panel_ota

**Tier:** services (ESP-IDF, board-agnostic) · **Features:** [13 OTA updates](../../../docs/features/13-OTA-Updates/README.md), [12 C6 co-processor firmware](../../../docs/features/12-C6-Coprocessor-Firmware/README.md) (the update path) ·
**Tests:** the portable helpers are host-tested in [test_fw.c](../../core/alarm_proto/test/test_fw.c) (`alarm_proto`'s versions, release manifest, install policy, serial framing and update entity, plus the retry schedule and signed-profile rule this component uses, `alarm_proto/ota_retry.h`
and `alarm_proto/ota_profile.h` since docs/features/33 step 8; `OTA-002`, `-004`, `-010`, `-013`, `-014`, `-015`). On a panel: the `ota` and `c6` console commands, `tools/ota_serial.py`, `tools/panelctl.py flash`, and the owner-authorised scripts in [tools/tests](../../../tools/tests) (`hardware_remote.py` rollback, `hardware_tamper.py`, `panel_web_ota_hil.py`).

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
most 180 s) or the image marks itself invalid and the previous one returns. After a change of
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

## Public API
| Header | What it offers |
|---|---|
| `include/panel_ota.h` | `panel_ota_start(required)`, the heartbeat contract (`panel_ota_healthy()`, `panel_ota_health_stale()`, `PANEL_OTA_OK_*` bits), status and listeners, the update server (check, commit, channel, facts, wanted locales, the running version's languages) / check / install / stop / cancel / discard, the writer (`panel_ota_begin/write/end/abort/apply`), `panel_ota_system_idle()` and the gate that answers it on a panel with no engine of its own (`panel_ota_set_idle_gate()`, a link keypad, docs/features/11), remote-development probation (`panel_ota_remote_prepare/confirm/operation`, `panel_ota_cancel_remote()`), `panel_ota_cp_update()`, `panel_ota_console_register()` |
| (in `alarm_proto`) `alarm_proto/ota_profile.h`, `alarm_proto/ota_retry.h` | which signed project names may replace the running one; the retry schedule after a failed check (moved there in docs/features/33 step 8 so they are host-tested as part of a core component) |
| `panel_ota_internal.h` (private) | shared between `panel_ota.c`, `cp_ota.c`, `ota_console.c` and `ota_trust.c` |

## Depends on
- Project components: `alarm_core` (public: `alarm_types.h`), `platform_esp` (`alarm_service` for the idle gate and firmware events, `panel_log` flush), `alarm_proto` (`proto_fw`: versions, install policy; `proto_fwsrv`: the server's domain, check, keys and trust record), `alarm_mem` (slow memory).
- ESP-IDF: `esp_common` (public), `app_update`, `bootloader_support` (`verify_rsa_signature_block`), `console`, `esp_app_format`, `esp_driver_uart`, `esp_http_client`, `esp_partition`, `mbedtls`, `nvs_flash`; `espressif/esp_hosted` `~3.0.9` on the ESP32-P4 (`idf_component.yml`).

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
- NVS namespaces `ota` (`trust`: the server and its accepted key; `chan`: the channel; rollback note, co-processor attempt; the old `src` is no longer read) and `devota` (the remote probation record, `panel_ota.c:689`).

## Rules
- Never replace or commit `secrets/ota_signing_key.pem`: the panel accepts only images signed with it. Keep the signing and rollback options in `sdkconfig.defaults` (CONTRIBUTING.md).
- Never run a C6 co-processor update, or enable eFuse Secure Boot or flash encryption, unless the owner asks explicitly (AGENTS.md, "Working with a real panel").
- `panel_ota_health_t` bits: append, never renumber. Heartbeats come from the task that does the work, at least every 5 s (`panel_ota.h`).
- `panel_ota_system_idle()` waits for the alarm task: call it from the update or console task, never from a listener (`panel_ota_internal.h`).
- Changing `partitions.csv` means a full erase of the panel: avoid it (CONTRIBUTING.md).
- Planned (docs/features/33 step 8): the C6 part only with ESP-Hosted, the device name from the project, the partition labels from the table.
