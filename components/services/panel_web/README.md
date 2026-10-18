# panel_web

**Tier:** services (ESP-IDF, board-agnostic) · **Features:** [31 Browser Panel Manager](../../../docs/features/31-Browser-Panel-Manager/README.md), [14 Configuration and installer programming](../../../docs/features/14-Configuration-and-Installer-Programming/README.md#who-can-change-what) (who can change what), [30 Remote development](../../../docs/features/30-Remote-Development/README.md) ·
**Tests:** no C host test. The browser side is tested with Node against the mock panel (`web/mock-panel.cjs`): [panel_pages.test.cjs](test/panel_pages.test.cjs) (every page as installer and as master) and [panel_frames.test.cjs](test/panel_frames.test.cjs) (capture scheduling). On a panel: the `web` console command and the scripts in [tools/tests](../../../tools/tests) (`panel_web_hil.py`, `panel_web_configuration_hil.py`, `panel_web_modes_hil.py`, `panel_web_ota_hil.py`, `panel_web_fps_hil.py`, `panel_web_browser_test.py`), each run only on an explicitly selected panel.

The Panel Manager: the panel serves its own browser interface over HTTPS on port 443, with no
desktop bridge. It embeds [`web/`](web/README.md)'s `panel.html`, `panel.css`, `panel.js`,
`panel-pages.js` and `panel-keypad.js` (the `mock-*.cjs` there are PC only and are not
embedded), each gzipped at build time by `gzip_asset.py` (deterministic: no name or
time in the header) and sent with `Content-Encoding: gzip` to a client that lists gzip in
`Accept-Encoding`; any other client gets the file inflated on the way out by the ROM's `tinfl`
(a 32 KiB window from PSRAM, chunked), under its own ETag. `/i18n/<locale>.json` is a language's
words for the pages, written from the image's own catalog in 4 KB pieces (`i18n_web_json`, no
file in the image), and `/i18n/panel` names the panel's language; both need no session, as the
sign-in page is worded too. A request's `X-Panel-Language` words what the panel makes for the
page - events, zone states, check lines, answers - in that language (docs/features/37 I18N-031).
It answers a JSON API under `/api/v1`: status and history, arming and disarming as the
signed-in person, the walk test (`GET walk-test` and its `partition` actions, docs/features/40), typed and
allow-listed configuration (the same validation as the touchscreen, through `ui_backend_t`),
users, zone sources, Z-Wave (`web_zwave.c`: the radio, its devices, SmartStart's list, adding
and removing devices and their actions, the radio's diagnostics), automations
(`web_automations.c`, docs/features/41: `GET automations` - the rules with their last outcome,
the zones and partitions a trigger may name; `GET automations/devices` - the devices and the
actions an automation may send them, from `hc_auto_offer` with the installer option; `POST
automations` - save, switch, delete and Try it now through `automation_service`, as the
signed-in person, never by a workstation's grant; the system master's and the installer's, not
while armed),
network with a timed confirm, notifications, storage, signed firmware
upload through `panel_ota`, the full-resolution remote keypad (JPEG frames and pointer input
through the adapter) and developer enrolment. People sign in with a user name and a password
(`web_login.c`); keypad codes do not sign in. The Installer's Web server and Developer switches
are stored here and default to off on a fresh panel. Alarm operations run on the alarm task
through `alarm_service`; this component never touches the engine or LVGL itself.

## Public API
| Header | What it offers |
|---|---|
| `include/panel_web.h` | `panel_web_start(adapter)`, `panel_web_get/set()` (the Web server / Developer switches), `panel_web_ui_backend()` (adds the remote, sign-in and About entries to a `ui_backend_t`), `panel_web_issue_grant()` (one-time grant for the enrolled `dev_remote` client) |
| `include/web_login.h` | `web_login_init()`, `web_login_listener()`, eligibility, set / clear / temporary / change password, `web_login_check()`, `web_login_verify()` (a recent password), `web_login_panel_id()`, `web_login_fingerprint()` |
| `web_config.h`, `web_extra.h`, `web_mem.h` (private) | typed configuration fields; hardware and service pages, and the wrong-guess limiter (`web_gate_t`); per-request slow-memory allocation |
| `web_automations.h` (private) | the Automations page's JSON (docs/features/41 DESIGN.md section 10); a stub that answers nothing without `ALARM_FEATURE_AUTOMATIONS` |

## Depends on
- Project components: `alarm_ui` (public: `ui_app.h` types, and `ui_port_esp_backend()` for the backend), `dev_remote` (public: the adapter type and `dev_cert.h`), `platform_esp` (`alarm_service`, `panel_log`, `net_wifi`, `panel_prefs`, `panel_storage`, `panel_system`, `zone_service`), `panel_ota`, `net_mqtt` and `net_notify` (CA upload), `automation_service` and `device_service` (the Automations page), `notify` (event sentences), `alarm_core`, `alarm_mem`.
- ESP-IDF: `esp_https_server`, `espressif__cjson` (`espressif/cjson` `^1.7.19`, `idf_component.yml`), `esp_timer`, `esp_hw_support`, `app_update`, `mbedtls`, `nvs_flash`, `console`, `esp_driver_jpeg` (listed; no source here includes it).

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `ALARM_PANEL_WEB` (`firmware/crowpanel_p4/main/Kconfig.projbuild`, no prompt) | y | With `ALARM_DEV_REMOTE`, compiles the firmware's `dev_adapter.c`, which starts this service, and the board's screen capture. Also set in `sdkconfig.defaults.release` |
| `ALARM_FEATURE_AUTOMATIONS` (`alarm_features`) | y with PSRAM | The Automations page: `capabilities.automations` in `status`; without it the three paths answer 404 |
| `ALARM_FEATURE_MQTT` / `ALARM_FEATURE_NOTIFY` | y | Gate the CA upload for MQTT and ntfy (`web_extra.c:433`, `:437`); the pages follow the backend entries the UI port fills |
| `ALARM_SERVICE_CONSOLE` (`platform_esp`) | y (off in release) | Registers `web` (status, on/off, developer, `password <user> [off]`) |

The component's `CMakeLists.txt` fails the build unless `CONFIG_CJSON_NESTING_LIMIT <= 12`
(set in `sdkconfig.defaults`), so public JSON parsing stays bounded on the worker's stack.

## Resources
- HTTPS server while Web server is on: 16384-byte worker stack, priority 2, 3 sockets with LRU purge, control port 32770 (32769 is `dev_remote`'s) (`panel_web.c:1292-1301`); one TLS session per socket.
- Task `panel_web` (starts and stops the server; no flash writes): 6144-byte slow stack via `alarm_mem_task_create()`, priority 1 (`panel_web.c:1436`).
- Slow memory (`alarm_mem`, PSRAM on the P4, no fallback): every per-request JSON tree (process-wide cJSON hooks, `panel_web.c:1374-1384`), configuration copies and log pages (`web_mem.h`), the certificate PEM and the 4 sessions (`:53`, `:62`), the sign-in records (`web_login.c:43`). Request bodies are limited to 48 KiB (`BODY_LIMIT`, `panel_web.c:42`).
- Sessions: 4, 20 min idle, 8 h at most (`panel_web.c:41-44`).
- NVS namespaces `panelweb` (switches, TLS identity, `:1397`) and `weblogin` (PBKDF2-HMAC-SHA256 records, 20,000 rounds, `web_login.c:24-25`).

## Rules
- Every permission is checked again here, whatever the browser shows; access follows docs/features/14 "Who can change what" (`panel_web.h`).
- The HTTPS worker writes flash (uploads, NVS), so its stack stays internal; only the maintenance task has a slow stack (`web_mem.h`; CONTRIBUTING.md, "Rules for the firmware").
- No secret crosses the wire: configuration is serialised from explicit field tables, never raw structures, salts or hashes (`web_config.c`).
- Try a page change against `components/services/panel_web/web/mock-panel.cjs` before a panel (CONTRIBUTING.md).
- Planned (docs/features/33 step 5): `panel_web` stops requiring the UI once `ui_backend` is a core component and the ESP port moves to `services/ui_port_esp`.
- Planned (docs/features/33 step 6): the firmware requires optional components only when their feature is on.

### Keypads and headless masters (BL-048)

The approved [partition and enrollment flow](../../../docs/features/31-Browser-Panel-Manager/keypad-enrollment.md) uses `web_keypads.c`. The HTTPS worker owns attempts; queued maintenance work expires them even without browser polling. `plink_service` exposes its management calls without the UI feature. The browser never invents a keypad record: both pairing confirmations must complete before its setup popup. Host tests compile this production service in both UI and headless modes.
