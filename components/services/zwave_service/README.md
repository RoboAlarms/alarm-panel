# zwave_service

**Tier:** services (ESP-IDF, board-agnostic) · **Features:**
[36 Z-Wave sensors](../../../docs/features/36-Z-Wave-Sensors-ZAC93/README.md) (milestone Z8),
[22 Zone sources](../../../docs/features/22-Zone-Sources/README.md),
[30 Remote development](../../../docs/features/30-Remote-Development/README.md) ("Z-Wave") ·
**Tests:** everything but the RTOS is host-tested: the network, the pages' view and
requests, the log and the stored formats ([zwave_zones](../../core/zwave_zones/test) `zwz_panel`),
the library below it ([zwave_host](../../core/zwave_host/test)); the UART, NVS and PSA are checked
on a panel with the console's `zwave` command (Z9), the PSA crypto port also by its self-test at
every start; the developer link's JSON and frame trace on a panel with `tools/panelctl.py` (Z9)

Z-Wave sensors through a Zooz ZAC93 on a UART: the panel runs its own Z-Wave network (it is the
only controller in it) and its devices are zones. The service is the RTOS around
`components/core/zwave_zones`' `zwz_panel`, which holds the network (`zw_net`), the bridge to the
zone source (`zsrc_zwave`), what the Z-Wave pages show and ask for, the logged events and what is
stored. It also answers the enrolled workstation over the developer link (docs/features/30): the
state and requests as JSON, and in development builds a trace of every byte on the wire.

## Public API
The service also forwards alarm events/state to `zw_feedback` (feature36 ZW-028). Alarm
listeners use separate bounded event and latest-state mailboxes; the network owner sends
Indicator commands under the zone lock. Device-owned audio has no panel voice dependency.
The console's `keypad-feedback` subcommand lists/assigns/removes output-only slots, persisted
in NVS `zwave/keypad_fb` before taking effect. Assignments require a quiet/disarmed panel and
a supported secure device; incoming keypad control is still separate unfinished work. The
device page's "Sounds and lights" (`UI_ZW_REQ_FEEDBACK`) takes the same path: the touch UI's
request becomes `ZWSVC_REQ_FEEDBACK` with `ZWSVC_SLOT_ANY` (the device's own slot, else the
first free one; the generation the page showed must still be the node's), installer mode and
disarmed; `zwz_panel_set_feedback()` lets the page read its fit and assignment.
`zwave setup <n>` continues a stopped interview and `zwave remove-failed <n>` is the page's
"reset, broken or gone" removal (docs/features/36 ZW-029, docs/bugs/B015).

| Header | What it offers |
|---|---|
| `include/zwave_service.h` | `zwave_service_start()` (`zwave_service_config_t`: the place, TX/RX, the reset line), `zwave_service_rules()` / `zwave_service_stored_place()` / `zwave_service_stored_reset_pin()` (`zwave place`, `zwave reset-pin`: the app decides what the board allows, hardware.md §4), `zwave_service_console_register()` (the `zwave` command), `zwave_service_ui_status/devices/device/request/progress()` (the touch UI's `zw_*` calls, for `ui_port_esp`), `zwave_service_state_json()` / `zwave_service_trace_json()` (development builds) / `zwave_service_request_json()` with their bounds `ZWAVE_SERVICE_*_JSON_MAX` (the developer link, wired by the firmware's `dev_adapter.c`), `zwave_service_ha_devices/_ha_command()` (Home Assistant over MQTT and ha_link), `zwave_service_output_post()` / `zwave_service_link_up()` (device_service's Z-Wave source: a command posted to the zwave task's 4-deep queue, answered once by callback, never waited for; docs/features/41 phase B) |

## Depends on
- Project components: public `ui_backend`; private `zwave_zones`, `zwave_host`, `zwave_proto`, `zone_hub`, `alarm_core`, `alarm_proto` (the strict JSON reader and the writer, for the developer link), `platform_esp` (the zone service's lock and sink, the alarm service), `alarm_mem`, `board_info` (`board_custom_parse`)
- ESP-IDF: private `esp_driver_uart`, `esp_driver_gpio`, `mbedtls` (PSA), `nvs_flash`, `console`, `esp_app_format`, `esp_timer`

## Options
| Kconfig (`Kconfig`) | Default | Effect |
|---|---|---|
| `ALARM_ZSRC_ZWAVE` | n (the P4's manifest: y) | The service; off, its sources compile to nothing and callers guard their calls (the libraries it requires are built but never linked). On, the radio still touches no pin until `zwave place` (or the Flasher's Connected parts) chooses one |
| `ALARM_ZWAVE_S0` | n | Phase 3 (security.md §10); nothing yet |
| `ALARM_ZWAVE_LR` | n | Phase 3 (network.md §13): 16-bit node IDs where the region has Long Range |
| `ALARM_ZWAVE_UART_NUM` | 3 | The UART controller (UART0 is the console, UART2 the ESP32-H2's) |
| `ALARM_BUILD_DEV` (the firmware's profile) | n | The frame trace for the developer link (`zw_trace.c`) and the console's `zwave trace`; every P4 profile has `ALARM_ZSRC_ZWAVE` on (the manifest) |

## Tasks and memory
- `zw_link`: 4 KB internal stack, priority `ALARM_TASK_PRIORITY + 1`. Owns the UART (115200 8N1, 1 KB
  driver buffers each way, 20 events); ACKs a good frame before anyone else sees it, NAKs a
  damaged one, and NAKs a good one the zwave task has no room for (the module sends it again).
  Waits on a queue set: the UART's events and a doorbell for the zwave task's writes.
- `zwave`: 8 KB **internal** stack (it writes NVS: docs/bugs/B001), priority
  `ALARM_TASK_PRIORITY - 2` (below the zone task). Runs `zwz_panel` with the zone service's lock
  held, the requests one at a time, then NVS and the log after unlocking. Wakes at its next
  deadline, on news, or every second.
- PSRAM (`ALARM_SLOW_BSS`), 163 KB on the P4: `zwz_panel_t` (84 KB), `zsrc_zw_t` (23 KB), the
  node cache's encoding buffer (51 KB, the cache at its fullest), the link's message buffers
  (4 KB in, 1 KB out). Internal RAM: the two tasks' stacks (12 KB, allocated at start), about
  0.7 KB of state (the link's frame parser, the request slot) and the app's copies of the board
  description with the radio placed (0.65 KB, `panel_app`).
- Requests (UI, console and the developer link) go to the zwave task through one slot and wait up
  to 2 s for the answer; statuses are read under the zone service's lock from the caller's task.
- Development builds: the frame trace, 32 KB of PSRAM (1,024 entries and 16 KB of their bytes)
  and the link's copy of the frame being collected (260 B of PSRAM, so a damaged or cut-off frame
  can be recorded); a few dozen bytes of internal RAM. The developer link's answers use the
  caller's buffers and a device list from PSRAM for the moment of the call.

## Storage (NVS namespace `zwave`, network.md §2; the formats in `zwave_zones/zwz_panel.h`)
`keys` (generated from the hardware RNG at the first start, never logged or shown), `gen` (the
inclusion counter, written before `nodes`), `nodes` (the node cache blob), `cfg` (the region
chosen, new devices' check-in, S0 allowed), `chk` (devices' own check-ins), `place`
(`socket`/`j7`/`custom`, absent: off), `rstpin` (the reset line's GPIO, absent: none), `c_tx` `c_rx`
`c_reset` (`u8`, a custom place's pins, ZW-046: the place erased first and written last; leaving
custom forgets them and `rstpin`). A failed write is
retried every 30 s and raises the storage trouble, as the zone bindings do.

## Console (`zwave`)
`status` (with "crypto self-test: passed/FAILED") · `place [off|socket|j7]` ·
`place custom tx=<g> rx=<g> [reset=<g>]` (ZW-046: replies start "place custom from the next restart
(`restart`)" or "not custom: "; read back as "custom tx=.. rx=..[ reset=..]" in `status` and
`place`) · `reset-pin [<gpio>|off]` (refused while the place is custom) · `restart` · `reset` · `add` ·
`remove` · `stop` · `allow [access auth unauth]` · `pin <5 digits>` · `nodes` · `node <n>` ·
`ping <n>` · `interview <n>` · `checkin <n> 1h|4h|12h` · `region [<name>]` · `jam` (phase 2) ·
`trace on|off` (development builds) · `reset-network confirm`. Every command that changes the
network refuses while a partition is armed.

## The developer link (docs/features/30 "Z-Wave")
`zwave_dev.c` answers `GET /dev/v1/zwave` (the radio, the devices, the operation running: the
pages' `ui_zw_*` view in the contract's words) and `POST /dev/v1/zwave` (a request `{"op":...}`
from an allowlist of the pages' and the console's operations, through the same slot with the
console's `allowed`: every partition disarmed; the network reset needs `"confirm": true`; the
body goes through `alarm_proto`'s strict reader). `zw_trace.c` answers `GET
/dev/v1/zwave/trace`: the zw_link task records every frame, ACK/NAK/CAN and text run each way
(a damaged or cut-off frame as it came) in a ring the HTTPS worker reads one entry at a time
under a mutex the link waits for 5 ms at most. The words and bounds are in docs/features/30.

## Rules
- Nothing but the zwave task changes `zwz_panel`, and only with the zone service's lock held;
  the pages and the console only read it, under the same lock. No LVGL call anywhere here: the
  UI polls.
- The radio stays off - no pin configured - until a place is chosen; the zone source is
  registered either way, so zones bound to Z-Wave devices keep their bindings.
- The crypto port on PSA is proved once at start, before the network is set up
  (`zw_crypto_port_check`, ZW-008: FIPS-197 C.1, RFC 7748 §5.2 and §6.1, two random draws).
  Failed - or PSA would not start - the error is logged and the radio stays off as with no place
  (the zone source registered, no pin touched); `zwave status` says "crypto self-test: FAILED".
  The pages then show the radio as not connected: they have no word for this yet.
- Place and reset line apply from the next restart: the wired terminals are handed out once, at
  start, and the app takes the radio's pins out of them first (`board_zwave_place_on`,
  `board_zwave_reset_on`).
- The reset line is open drain: low for 20 ms, then released - never driven high.
- The network keys never reach the log, the console, a page or the developer link. The frame
  trace holds wire bytes only (S2 stays encrypted); S0 inclusion would put its network key on the
  wire under an all-zero key, so a development build with `ALARM_ZWAVE_S0` refuses to compile the
  trace until S0's key exchange is kept out of it.
- The developer link sends no frame of its caller's choosing: only the allowlisted requests, which
  the panel refuses by its own rules.
