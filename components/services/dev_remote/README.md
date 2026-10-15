# dev_remote

**Tier:** services (ESP-IDF, board-agnostic) · **Features:** [30 Remote development](../../../docs/features/30-Remote-Development/README.md), [31 Browser Panel Manager](../../../docs/features/31-Browser-Panel-Manager/README.md) (which supersedes Feature 30's release exclusion) ·
**Tests:** no host test of its own. The workstation side is tested by [test_panelctl.py](../../../tools/tests/test_panelctl.py) (`DEV-003/004/006/007/014`), and the signed-profile rule it relies on (`alarm_proto/ota_profile.h`) by [test_fw.c](../../core/alarm_proto/test/test_fw.c). On an enrolled panel: `tools/panelctl.py info|flash|screen|diagnostics|logs`, the `dev` console command, and the owner-authorised hardware scripts in [tools/tests](../../../tools/tests) (`hardware_auth.py`, `hardware_load.py`, `hardware_remote.py`, `hardware_tamper.py`).

The enrolled-workstation development service. It generates the panel's own P-256 TLS
identity, accepts exactly one enrolled client certificate (public certificates only, no bearer
defaults) and serves an HTTPS API on port 6055 under `/dev/v1`: identity and health, a log
ring, diagnostics, screen frames, exclusive remote touch control, a signed image upload with
install, confirm and cancel through `panel_ota`, and one-time browser grants for the Panel
Manager, the voice pack's and the asset packs' installs and states (`/dev/v1/voice`,
`/dev/v1/asset`, docs/features/30); with an adapter that has Z-Wave, the radio's state, requests and frame trace
(`/dev/v1/zwave`, docs/features/30 "Z-Wave"). It advertises `_roboalarms-dev._tcp`
over mDNS once Wi-Fi is up. It knows no board, no Z-Wave and draws nothing: screen capture,
touch injection, metrics and the Z-Wave JSON come through the caller's `dev_remote_adapter_t`
(the firmware's `main/dev_adapter.c`). The listener runs only while a client is enrolled and the
Installer's Developer switch allows it (`dev_remote_set_allowed()`, driven by `panel_web`).

## Public API
| Header | What it offers |
|---|---|
| `include/dev_remote.h` | `dev_remote_adapter_t` (board family, resolution, frame, control, renew, input, metrics, JPEG, frame interval, preference and grant callbacks; the remote keypad's; the optional Z-Wave `zwave_state`, `zwave_trace`, `zwave_request`), the Z-Wave routes' bounds `DEV_ZWAVE_STATE_MAX` / `_TRACE_MAX` / `_BODY_MAX` / `_ANSWER_MAX`, `dev_remote_start()`, `dev_remote_set_allowed()`, `dev_remote_enroll_public()`, `dev_remote_public_identity()`, `dev_remote_copy_log()` |
| `dev_cert.h` (public: `panel_web` uses it too) | `dev_cert_ensure()`: load or generate the panel's certificate and key in an NVS handle |

## Depends on
- Project components: `platform_esp` (`alarm_service` snapshot, `code_hash` fault counter, `net_wifi`, `panel_log`, `zone_service`), `panel_ota` (upload, profile check, remote probation), `zigbee_service` (`/dev/v1/zigbee`), `lang_packs` (`/dev/v1/asset`: `asset_packs_install()`, `asset_packs_list()`), `alarm_core` and `alarm_proto` (names and types), `alarm_mem` (slow memory).
- ESP-IDF: `esp_common` (public), `app_update`, `console`, `esp_app_format`, `esp_https_server`, `esp-tls`, `esp_timer`, `esp_hw_support`, `mbedtls`, `nvs_flash`, `espressif__mdns` (declared by `ha_link`'s `idf_component.yml`, not by this component).

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `ALARM_DEV_REMOTE` (`components/services/alarm_features/Kconfig`, no prompt) | y | With `ALARM_PANEL_WEB`, compiles the firmware's `dev_adapter.c`, which starts this service and `panel_web`, and the board's screen capture. Also set explicitly in `sdkconfig.defaults.dev` and `.release` |
| `ALARM_BUILD_DEV` / `ALARM_BUILD_RELEASE` (same file) | n / n | The `profile` field of `/dev/v1/info`; the firmware's adapter hands over the Z-Wave trace in dev builds only; `GET`/`POST /dev/v1/speaker` (the bench silence, docs/features/08 AUD-025), `PUT /dev/v1/play` and `POST /dev/v1/say` (the audio bench and the voice bench's words from the pack, AUD-030) and `POST /dev/v1/voice` (`volume <0-100>`: the Voice volume, saved as the Settings slider saves it) exist in dev builds only |
| `ALARM_SERVICE_CONSOLE` (`platform_esp`) | y (off in release) | Registers the `dev` command (enrol, status, revoke) |

## Resources
- Task `dev_remote` (maintenance: listener restart, mDNS, a metrics line every 10 s): 6144-byte internal stack, priority 1; it writes NVS.
- HTTPS server when enabled: 16384-byte worker stack ("injected taps run UI handlers here"), priority 2, at most 3 sockets with LRU purge, 3 s send/receive timeouts, 5 s handshake limit, 24 routes at most (ten, Z-Wave's three, Zigbee's two, the voice pack's two, the asset packs' two, and in dev builds the speaker's two and the audio bench's three); one TLS session per socket.
- Slow memory (`alarm_mem`, PSRAM on the P4): a half-resolution frame buffer, the certificates and the 32-line log ring (`ALARM_SLOW_BSS`), the console's text buffer; for the moment of a Z-Wave call, its answer (32 KB for the state, 12 KB for a trace). The private key stays in internal RAM.
- NVS namespace `devrem` (identity, enrolled `peer`); `panel_ota` keeps the remote probation record in `devota`.

## Rules
- No network work under a UI lock; LVGL is touched only by the adapter in the firmware, which takes `board_ui_lock()` (`dev_remote.h`; CONTRIBUTING.md, "Rules for the firmware").
- One enrolled client at a time. Enrolment and revocation are explicit (`dev` on the console, or the installer's Panel Manager page through `dev_remote_enroll_public()`); revoking or switching off access cancels a remote install still waiting for idle (docs/features/30).
- A network update must match the running image's project name, i.e. board and profile.
- Z-Wave only through the adapter: its routes are registered only when its callbacks are there, answer within the `DEV_ZWAVE_*` bounds, and pass nothing to the radio but the adapter's allowlisted requests; `"zwave"` joins `info`'s capabilities with them (docs/features/30).
- Zigbee straight from `zigbee_service` (docs/features/20 devices.md §12): `GET`/`POST /dev/v1/zigbee` are always registered and answer 404 on a panel without Zigbee; bounded by `ZIGBEE_SERVICE_*_JSON_MAX`; no network reset (its installer code never travels the network); `"zigbee"` joins `info`'s capabilities when the panel has it. The server's URI handler table is 22 (the speaker's two and the audio bench's three in dev builds).
- Tasks that write NVS or flash keep internal stacks (CONTRIBUTING.md).
- Planned (docs/features/33 step 6): `ALARM_DEV_REMOTE` becomes a real feature option that leaves this component and screen capture out when off.
