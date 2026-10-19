# zigbee_service

**Tier:** services (ESP-IDF, board-agnostic) · **Features:**
[20 Zigbee sensors](../../../docs/features/20-ESP32-H2-Sensor-Radio/README.md),
[22 Zone sources](../../../docs/features/22-Zone-Sources/README.md) · **Tests:** the bring-up policy and
the source are host-tested ([zigbee_host](../../core/zigbee_host/test),
[zone_hub](../../core/zone_hub/test) `zigbee`); the radio is checked on a panel with `zigbee`

The ESP32-H2 sensor radio as a zone source: bringing the radio up over the socket's UART
(the policy in `components/core/zigbee_host`), running Espressif's Zigbee stack
(`esp-zigbee-lib` 2.0.x) as the network's coordinator on top of it, and handing what it hears to
the Zigbee source (`radio_port.c`). The UART pins come from the app (`board_info_t.radio`); the
H2 runs `firmware/radios/h2_rcp`. Runs on a bench panel with an ESP32-H2 (radio answers, stack
starts, add window opens and closes); acceptance with real sensors is docs/features/20 "Next
bench acceptance". Writing the H2's flash from the panel is not built.

Since ZB-M6 (BL-143) the service owns zigbee_zones' `zbz_panel` (docs/features/20 devices.md):
the registry and install codes stored in NVS `zbpanel` (loaded before the radio starts, written
by the zigbee task after unlocking; a store NVS can't read keeps the radio stopped, the storage
trouble raised and the stored-device changes refused, and is read again later or on `zigbee
retry`, after which the sirens and keypads are told the engine's state again), every device's setup, the operations the touchscreen's
Zigbee pages (`zigbee_service_ui_*`, through ui_port_esp), the console and the developer link
ask for, the events it logs (ALARM_EV_ZIGBEE_*), the sirens and keypads (the engine's news from
panel_app's listener; keypad and fob commands run on the alarm task) and MQTT's
`~/zigbee/state` and zone diagnostics (through panel_app's `net_mqtt_zb_port_t`).

Console (`zigbee_console.c`, devices.md §12): `zigbee [status|retry|restart|devices|device <id>|
device <id> signature|trace on|off|add|stop|keep|remove <id>|forget <id> confirm|replace <id>|identify <id>|ping <id>|setup <id>|
rename <id> [name]|codes|code <ieee> <install code>|code forget <ieee>|survey|channel <n> confirm|
reset confirm <installer code>|progress|dismiss]`. `status` adds the radio's state, the latest
energy scan, the zigbee task's stack high-water mark and NVS's free entries. The bench's `device <id>
signature` (a testing.md §2 fixture row) and `trace on|off` (`zbtrace` lines for a replay) are in
`zigbee_bench.c`. The developer link's
`/dev/v1/zigbee` (`zigbee_dev.c`, served by dev_remote; `tools/panelctl.py zigbee`).

Split out of `platform_esp` in docs/features/33 step 6, together with the `zigbee` console command.

## Public API
| Header | What it offers |
|---|---|
| `include/zigbee_service.h` | `zigbee_service_start()` (`zigbee_service_config_t`: UART pins), `zigbee_service_source()`, `zigbee_service_status/retry()`, `zigbee_service_set_socket_taken()` (the Z-Wave radio holds the socket: no start, status says so), `zigbee_service_console_register()` (the `zigbee` command); since BL-143 `zigbee_service_set_report_sink()` (the radio side's `zsrc_zb_report_t`, for `zbz_panel_on_report`), `zigbee_service_wake_window()` (fast polling at a sleepy device's next check-ins), `zigbee_service_restart_radio()`; since ZB-M6 the pages' `zigbee_service_ui_status/devices/device/progress/codes/request()`, `zigbee_service_device_of_zone()`, `zigbee_service_changes()`, the engine's news `zigbee_service_engine_output/snapshot()`, MQTT's `zigbee_service_mqtt_state()` and `zigbee_service_zone_diags()`, the developer link's `zigbee_service_state_json()` / `zigbee_service_request_json()`; the keypad numbers 1-8 shared with the panel link (docs/features/11 LINK-001): `zigbee_service_devices_held()` (under the zone lock) and `zigbee_service_set_link_devices()` (taken by Zigbee's next choice at once) |

## Depends on
- Project components: public `zigbee_host`, `zone_hub`, `zigbee_zones`, `alarm_core`,
  `ui_backend`; private `platform_esp` (zones task, alarm_service), `alarm_mem`, `alarm_proto`
  (the strict JSON reader and writer)
- ESP-IDF: private `console`, `esp_driver_uart`, `esp_timer`, `nvs_flash`
- Registry: `espressif/esp-zigbee-lib` ~2.0.4 (`idf_component.yml`), fetched only when
  `ALARM_ZSRC_ZIGBEE` is on (a Kconfig condition; every P4 image, not the 4-inch board): it is
  170 MB for every chip it supports

## Options
| Kconfig (`Kconfig`) | Default | Effect |
|---|---|---|
| `ALARM_ZSRC_ZIGBEE` | n | The service, its task and the stack; off, it answers "not in this firmware" (stubs). On in the P4's `sdkconfig.defaults` (ADR-047), which also sets the stack's `ZB_ENABLED`, `ZB_ZCZR`, `ZB_RADIO_SPINEL_UART` |
| `ALARM_ZB_UART_BAUD` | 460800 | The socket's UART; must match the H2's `CONFIG_RCP_UART_BAUD` |
| `ALARM_ZB_CHANNEL_MASK` | 0x02108800 | Channels the network may form on (11, 15, 20, 25) |

## Resources
- Task `zigbee`: 8192 bytes, internal (it writes NVS `zbpanel`), priority
  `ALARM_TASK_PRIORITY` - 1: the bring-up policy, the queue of what the stack heard into the
  source and `zbz_panel` under the zone lock, the panel's polls and requests, then the store, the
  log and the keypads' commands after unlocking. `zigbee status` shows its high-water mark.
- `zbz_panel_t` (about 20 KB) and the source (about 5 KB) in PSRAM (`EXT_RAM_BSS_ATTR`), with the
  request slot, the store's buffers and the keypads' queue.
- Task `zb_stack`: 8192 bytes, internal (the stack writes NVS from it), priority 5: the stack's
  main loop and every callback. Queue of 32 events in PSRAM (about 180 bytes each since BL-143:
  an event carries a `zsrc_zb_report_t`); a 1 s retry timer.
- BL-143 tables in `radio_port.c`: the 16 requests waiting for their end (`s_rq`, PSRAM) and
  small internal ones (link quality of the last 8 senders, install codes held for 16 devices,
  8 joins, 8 wake windows), touched only with the stack's lock held; the sequence numbers of
  the panel's last 16 untagged ZCL sends (`s_own_tsn`, PSRAM, the stack's task only).
- Flash: the stack keeps its network in the panel's `nvs` partition, in its own namespace.
- Image: about 320 KB more than without Zigbee; static internal RAM 14 KB more (2026-09-28).

## Rules
- The socket's IO12/IO14 share the touch panel's I2C bus on the CrowPanel: radio firmware must
  leave them alone (`board_pins.h`).
- Off means stubs: callers need no guard (components/services/alarm_features).
- Lock order: the zone service's lock may be held while taking the stack's
  (`esp_zigbee_lock_acquire`), never the other way round; the stack's callbacks only queue.
- The UART is ours only until the RCP has answered the version probe; then it is the stack's.
- The stack's Spinel client (OpenThread, in ESP-IDF) ends the program when the RCP fails a
  reset, is incompatible or stops answering for good: the panel restarts. The restart guard in
  `radio_port.c` (`RTC_NOINIT_ATTR`, kept through a restart) counts starts the radio ended; after
  two in a row the stack stays off (`zigbee status` says so) until `zigbee retry`, so a flaky
  module can't keep the panel restarting. Ten minutes of a running network, or a power-on,
  clear the count.
- Restarting the stack in one boot leans on linker wraps in `radio_port.c` (CMakeLists.txt,
  docs/bugs/B024): on P4 v1.x builds the library's task-queue post (it reads v3-only CSRs), and
  in every build OpenThread's `SpinelDriver::Init` (reset first: ESP-IDF never does) and
  `esp_radio_spinel_sleep` (a stop during a transmission). After `esp_zigbee_deinit` the stack
  task frees the panel's device descriptor, which the deinit leaves. Check them at each
  esp-zigbee-lib or ESP-IDF update.
- The radio's pending table (docs/bugs/B021): esp-zigbee-lib 2.0.4 links its single-address
  clears (`esp_radio_spinel_clear_short_entry`, `_extended_entry`) to weak stubs of its own that
  do nothing, so `radio_port.c` defines them, as ESP-IDF does after v6.1.0 (commit 68d2d8c4), on
  the RadioSpinel it learns from wraps of the add calls. An ESP-IDF that has them fails to link
  ("multiple definition"): then drop ours and the two wraps. The H2's half is
  `firmware/radios/h2_rcp/main/rcp_pending.c`.
- The panel's endpoint is a client of every cluster it reads or hears (Basic included): a
  sensor's answers travel server to client and reach only a client of their cluster.
- Joining opens only on request (the learn window, `zigbee pair`): no network steering after
  forming, nothing opened after a restart (ZB-002).
- Reports (BL-143, docs/features/20 devices.md §2-§3): the stack's callbacks queue them; the
  service's task hands them to the report sink with the zone lock held. The sink may be set
  or changed at any time (atomic: its context is stored before its function). A tagged request
  (a `zsrc_zb_radio_t` hook after `energy_scan`) refuses tag 0 and ends with one
  `ZSRC_ZB_REP_DONE`, after its answer's own report: ZDO requests by their callback, ZCL ones
  by the answer with the request's sequence number (or a Default Response), anything else by
  its timeout (20 s; a channel move 25 s, a network reset 55 s), and `NO_RADIO` when the stack
  stops (queued before the link goes down; the service's task keeps emptying the queue while
  the stack is down). The queue never blocks the stack: when it is full an event is dropped
  (counted, logged "radio events dropped"), a `REP_DONE` too, and `zbz_panel`'s own step
  timeouts end that step. An answer to one of the panel's own untagged sends (the join
  interview, the CIE write, the enrollment response) never ends a tagged request: their
  sequence numbers are noted from the send confirmation. LQI and RSSI come from the APS
  indication (`ezb_apsde_data_indication_handler_register`, which leaves every frame to the
  stack).
- The alarm path is never shed for anything else (ZB-003, ZB-024): the stack has already
  acknowledged a sensor's IAS zone status, its enroll request and a keypad's or fob's ACE command
  (`alarm_path()` in `radio_port.c`), so the device never sends them again. They may use every
  slot of the queue (32 + 8); any other event leaves the last 8 free, and a frame's "it is alive"
  (`EV_FRAME`, shed first) the last 12. One lost even so is counted apart (`zigbee status`
  "alarm events lost", the developer link's `radio.alarm_lost`, `panelctl zigbee`), logged as an
  error, and a lost zone status is read back from the device (a listening one answers at once; a
  sleepy one only if it polls in time, else its next change or supervision report sets it
  right). It also raises the panel's one "sensors may not be heard" trouble
  (`zone_service_report_lost`), held for 5 minutes after the last loss (`tell_lost`,
  `ZB_LOST_HOLD_MS`): nobody can say what a lost keypad command or enroll request was.
- Copies of a keypad's code: an ACE Arm's or Bypass's payload never enters the radio queue
  (whose PSRAM slots keep their bytes after a read); `report_zcl` parks it in one of 8 code slots
  (`code_park`) and the event carries the slot's number, the pump takes it back and wipes the slot
  (`code_take`), and both wipe their own copy of the event. All slots taken counts as an
  alarm-path event lost. The developer link wipes a request's
  body (an install code) whether or not it parsed.
- A request is answered with its own result (`zbsvc_submit`): 2 s to be taken, then 2 s more once
  the zigbee task runs it, so an add window opening or a device being removed never reads
  "busy" while it goes ahead; `BUSY` only when even that passed (counted, "timed out").
- The channel survey is refused while any partition is armed (`run_request`, ZB-016): its energy
  scans leave the panel deaf about 4 x 138 ms.
- While `reset_network` runs (until its `REP_DONE`), every hook that sends into the network
  refuses (returns false); the ones that touch only the panel's tables and the trust center's
  settings (install codes, codes only, forget, wake window, restart) still work.
- `ias_test_mode` takes seconds, as ZCL's Initiate Test Mode carries them (one byte,
  ias_zone.h:56): the walk test asks for 60 s and asks again every 45 s while it runs
  (`ZSRC_ZB_TEST_MODE_LEN`, `ZSRC_ZB_TEST_REFRESH_MS`), so a device left in test mode leaves it by
  itself within a minute.
- How a device joined: esp-zigbee-lib doesn't say which key a joiner used. `INSTALL_CODE` means
  the trust center held an install code for it when it was let in (only that key can read the
  network key it was sent), `STANDARD_KEY` that it held none; a rejoin is `UNKNOWN`. Inferred,
  and only right while two things hold: the panel's list of held codes (RAM, 16) matches the
  trust center's, which needs `zbz_panel` to give every waiting code again at each start
  (zbz_panel.h `zbz_panel_radio`); and the trust center never lets a device in with the
  well-known key while it holds a code for it. Must be checked on the bench (docs/features/20
  testing.md) before a device page shows the label as fact.
- With a report sink set, a keypad's IAS ACE commands reach the sink raw (the stack doesn't
  answer them): the panel's ACE policy answers with the request's own sequence number
  (`zcl_command`, sent as an APS frame; `ZSRC_ZB_TSN_NEW` takes a new one). The panel's endpoint
  carries the IAS WD client and IAS ACE server clusters. The pump wipes its copy of each report
  once delivered (a keypad's Arm carries its code).
- The join interview is zbz_panel's tagged setup alone (devices.md §5, decided in ZB-M6): the
  radio side no longer sends its own untagged reads after an announce.
- Not offered by the library, left as TODOs: `forget` drops the install code and the panel's
  own records but not the stack's address map entry or link key (devices.md open question 1);
  `set_channel` relies on the stack moving the coordinator after its Mgmt_NWK_Update broadcast
  and ends `TIMEOUT` if it doesn't. `reset_network` uses the local reset (no panel restart),
  never `esp_zigbee_factory_reset` (which restarts the panel).
