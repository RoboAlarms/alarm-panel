# plink_service

**Tier:** services (ESP-IDF, board-agnostic) · **Features:** [11 Panel link: master and remote keypads](../../../docs/features/11-Panel-Link-Master-Keypad/README.md) (BL-048) ·
**Tests:** the protocol, pairing, sessions, both link managers, the stored records and the keypads' view are portable and host-tested in [`panel_link`](../../core/panel_link) (`LINK-NNN`); `host/link_sim` drives the same managers over UDP on a PC (LINK-013). This component has no host test of its own. On a panel: the `plink` console command; the HIL cases are docs/features/11 "Test and acceptance".

The panel link on a panel. Every panel has a role, stored once: the **master** (the alarm panel,
and every panel before this feature) or a **keypad** for a master elsewhere in the house. One task
owns a UDP socket on port 6056 and the role's link manager from `panel_link`, which does the
protocol; this component gives it the network, NVS, PSA, the engine and the screen.

- **On a master** the alarm listener hands the engine's news to the link task (a snapshot flag,
  events and one-shot sounds in queues that never wait; sent as EVENTS, then SOUND, then the
  STATUS, so a keypad's voice has a batch's tone before its STATUS); a worker task runs the keypads' commands,
  code checks and code changes on the alarm task as `{ALARM_SRC_KEYPAD, n}`, reads each keypad's
  lockout after its command, rebuilds the keypads' copy of the configuration (no code material,
  LINK-019) and tells the engine about silent keypads (`alarm_service_device_reachable`, LINK-007;
  no check-ins: they would only repeat the edges, once per heartbeat, and the engine call can wait
  for the alarm task, which the link task never does).
  The link is announced by mDNS `_roboalarms-link._udp` only while a keypad is paired or pairing is
  open (LINK-021); while keypads are paired a lost network is the network trouble and Wi-Fi power
  save is off (LINK-024). The keypads' device numbers 1-8 are shared with the Zigbee keypads
  (LINK-001): the worker reads Zigbee's every second (it takes the zone lock, so the link task
  never does), and a new keypad's number is reserved with Zigbee and checked again without waiting
  before its pairing is answered (docs/features/11 "Open questions" names the window left).
- **Smart Home and settings on a master's keypads** (docs/features/11 "Smart Home and Home
  Assistant on keypads", BL-216; `plink_home_svc.c`). The master offers `PLINK_CAP_HOME` in a
  build with home control whose registry started (`plink_service_set_home`, from `panel_app`) and
  `PLINK_CAP_PREFS` in every build with the UI. Its `plink_dev` task copies the registry
  (`device_service_list`) and builds the device list and its records (`plink_home`) into two
  alternating PSRAM buffers - when `device_service_seq()` moves (at most every 250 ms), at once
  after a device action, and every 2 s to compare - only while a live keypad has HOME; a buffer
  is written only while neither the manager nor a build waiting for the link task holds it. It
  runs the keypads' device actions (8 slots) through the one judge of both screens,
  `device_service_person_act({ALARM_SRC_KEYPAD, n}, the keypad's home partition)` (LINK-048),
  wipes the code copy, and reads the keypad's lockout after an action that carried a code, so its
  STATUS shows one. The build after an action (RES_DEVICES) goes on the worker's result queue
  before the answer (RES_DEV_ACK), so the DEV_ACK's record shows the action under way.
  `plink_service_prefs_set` queues a settings write for the link task (4 deep, never waits;
  exactly one `done` follows once queued), and `plink_service_keypads` gives Home Assistant's
  keypads list (name, home, online, firmware, signal, the settings report, can, voice; only the id
  while the keypad is online in a session without PREFS) from what the link task publishes every
  0.5 s; a write's end publishes the keypad's report before its `done` runs. `panel_app` sets `ha_link`'s settings port onto these two.
- **On a keypad** (`panel_app` starts no engine: `alarm_service_set_dormant()`) the link task
  finds the master (mDNS, the subnet broadcast, or an address set at the console; again whenever
  the manager's handshake goes unanswered, `port.resolve`; the subnet broadcast, ip | ~netmask,
  is named as one to `plink_keypad_master_addr` and `plink_keypad_pair_start`, since its bytes
  don't say so on a subnet other than a /24), and pairs by that broadcast when mDNS finds no master
  open for pairing within 10 s (`PAIR_BROADCAST`; the first master's answer is taken), keeps the
  copies the touch UI reads, sends its commands (answered within 3 s, refused at once while the
  link is down, panic excepted), plays the master's sounds, follows its clock and time zone (no
  time server of its own while paired), answers the update gate, and tells the Emergency words the
  master's reporting (the backend's `mqtt_status` only, so the Integrations pages stay hidden).
  A fire or CO tone kept through a lost link stops on OFF keyed here that the master didn't run
  (link_info `local_alarm` asks Home for its Silence); a keypad that can't read its master's
  configuration says it needs an update (`needs_update`), and the gate lets that update in under
  the rule a connected keypad has. `panel_app` starts the link before the screen, so the UI's
  first look finds the keypad role.
  Its voice (LINK-028): with `plink_service_set_voice` (the six `panel_voice_engine_*` entries,
  set by `panel_app` before the start) the link task hands the keypad's voice what its master
  sends, through `plink_vfeed` after each turn's sounds and copies - a baseline at each ONLINE, a
  pause when the link isn't, covered events once, commits - and plays the master's one-shot
  sounds tracked while the voice is fed. `plink_service_voice_resync` (the voice wants a
  baseline) and `plink_service_keypad_config` (the configuration copy and its generation, for the
  zones' words) serve its source. The voice's Announcements, Voice volume and Preview stay in the
  keypad backend; the spoken-name editor's entries are off (the alarm panel's installer approves
  the words).
- **Smart Home and settings on a keypad** (docs/features/11 "Smart Home and Home Assistant on
  keypads", BL-216; `plink_keypad_svc.c`, `plink_backend.c`). In a build with home control and
  PSRAM the keypad allocates the core's mirror of its master's devices (`plink_dev_mirror_t`) and
  the screen's `ui_dev_t[64]`, attaches the mirror (`plink_keypad_home_attach`) and so offers
  `PLINK_CAP_HOME`; `PLINK_CAP_PREFS` in every build with the UI. Whenever the mirror moves (a
  list, a record, an action, its freshness) the link task makes the screen's copy again under its
  own `s_dev_lock` (never `link_pub_lock`): each usable row through `plink_keypad_dev_view` and
  the one converter of both screens, `ui_dev_from_parts`, every tile offline since the last device
  data while the link isn't ONLINE or none came for 30 s (LINK-046). The backend's `dev_list` reads
  that copy; its `dev_act` answers at once from it what it can (UNREACHABLE, REFUSED, NEED_CODE -
  the pad opens with no round trip), turns a thermostat's mode into its `DEV_MODE_*` bit, then
  hands the action to the link task through a box, mutex and semaphore of its own (never the
  commands') and waits 1 s, 3 s with a code. A refused code gives the pad its reason; no answer in
  time is `UI_DEV_OK`, the link task releasing the action so its tile shows it pending - failed
  for a late refusal, "Not confirmed" once given up - until a later record (LINK-047). The rail
  shows Smart Home under the master's rule; Arrange keeps `ui_port_esp`'s layout entries, each
  panel's own (decision 2). Settings: the link task reports them (`plink_keypad_prefs_report`,
  from `panel_prefs_current`, never NVS) when `panel_prefs_seq()` moves or the voice's state
  changes, with this panel's id and the fields it takes (the announcements' only with a voice in
  the build). A change from the master (PREFS_SET) is checked with `proto_prefs` (a field it
  doesn't take, out of range, of the wrong length, named twice or unknown is refused here), handed
  to the screen through `panel_prefs_remote_submit` (UI-046; the screen's `done` only copies its
  answer into a box) and answered with the screen's refusals and the values then stored
  (`plink_keypad_prefs_answer`); a change the screen doesn't take in 3 s is withdrawn and answered
  busy. The link task never takes `board_ui_lock` and never writes NVS for any of it.
- **The setup** of a panel that becomes a keypad pairs while it is still a master (Option C): a
  keypad manager of its own on the same socket; the setup's Done stores the record and the keypad
  role and restarts. Paired, it sends nothing more until Done (the other master keeps the keypad
  provisional, dropped after 10 minutes if the setup is abandoned), and its last step gets the
  home partition through `link_info`, never the other master's configuration. Leaving the keypad
  branch (`KP_PAIR_STOP`, or the setup done as the alarm panel) forgets the pairing, clears what
  it published and ends the manager (`ksvc_setup_end`); the master's own pairing is published
  again.

## Public API
| Header | What it offers |
|---|---|
| `include/plink_service.h` | `plink_service_role()`, `plink_service_force_master()` (a start that runs the alarm panel although a keypad role is stored); on a master `plink_service_listener()` (register before `alarm_service_start()`), `plink_service_set_devices_port()` (Zigbee's numbers), `plink_service_set_home()` (the registry runs: Smart Home offered), `plink_service_keypads()` and `plink_service_prefs_set()` (the keypads' settings for Home Assistant); `plink_service_start()`; on a keypad `plink_service_keypad_home()` (the UI's partition), `plink_service_keypad_power()` (the heartbeat's power flags) and `plink_service_update_idle()` (for `panel_ota_set_idle_gate`); `plink_service_extend_backend()` (the UI's `link_*` entries, and a keypad's link-served entries); `plink_service_console_register()` |
| `src/plink_internal.h` (private) | the link task's hand-overs and published copies, shared by the source files |

| Source | What it does |
|---|---|
| `plink_service.c` | the role, the link task, the socket, NVS `link`, mDNS start, the request hand-over |
| `plink_psa.c` | the crypto port (`plink_crypto_t`) on PSA: AES-128 ECB, X25519, the hardware RNG; proved by `plink_crypto_check` at every start; the cached AES key goes at the end of every link task turn (`plink_psa_forget`) |
| `plink_master_svc.c` | the master: listener, worker, the manager's port, mDNS, holds, the copies the UI reads |
| `plink_home_svc.c` | the master's Smart Home and settings for its keypads: the `plink_dev` task, its builds and device actions, the settings writes, Home Assistant's keypads list |
| `plink_keypad_svc.c` | a keypad: discovery, commands, sounds, time, the update gate and heartbeat, the UI's copies, Smart Home's mirror and the screen's devices and actions, its settings for the master; the setup's pairing (Option C) |
| `plink_backend.c` | the `ui_backend_t` entries |
| `plink_console.c` | the `plink` command |

## Depends on
- Project components: `panel_link` (the protocol and the managers), `platform_esp` (`alarm_service`, `net_wifi`, `panel_prefs`, `sounder`, `panel_log`), `panel_ota` (the heartbeat, the gate's answer), `ui_backend` (the UI's types), `alarm_mem`, `alarm_core`, `alarm_proto` (the panel id, `proto_fw_install_allowed`; public for `proto_link_keypad_t` and `proto_prefs`), `device_service` (the registry and the judge, on a master), `panel_voice` (a keypad's voice state in its settings report), `i18n`, and for the reporting state on a master `ha_link` and `net_mqtt`.
- ESP-IDF: `lwip` (one UDP socket), `esp_netif`, `nvs_flash`, `mbedtls` (PSA), `console`, `esp_app_format`; `espressif/mdns` `^1.8.0` from the component registry (`idf_component.yml`, already in both P4 lock files through `ha_link`).

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `ALARM_LINK` (`components/services/alarm_features`) | y with PSRAM | Off: nothing here is compiled ("absent"), `panel_app` neither registers the listener nor starts the link, every panel is a master without keypads and the UI's `link_*` entries stay NULL |
| `ALARM_BUILD_DEV` | n | On: `plink pair force` opens pairing without installer mode (a bench shortcut) |
| `ALARM_SERVICE_CONSOLE` | y (off in release) | the `plink` command |

The firmware sets `CONFIG_LWIP_UDP_RECVMBOX_SIZE=32` (`firmware/crowpanel_p4/sdkconfig.defaults`) so
a burst from eight keypads isn't dropped.

## Resources
- Task `plink`: 8192-byte internal-RAM stack, priority 5 (it writes NVS at pairing and removal and
  runs PSA's X25519). It waits in `select()` 50 ms at most.
- Task `plink_work` (master only): 4096-byte internal-RAM stack, priority 4; it waits for the alarm
  task, never the link task.
- Task `plink_dev` (a master offering Smart Home): 4096-byte PSRAM stack (`alarm_mem_task_create`;
  it never writes flash or NVS), priority 3, below `plink_work`; it waits for the alarm task (a
  code check) and the device lock, never the link task. Wakes every 250 ms while a keypad has
  HOME, every second otherwise. `plink` prints its stack's minimum free.
- PSRAM: the manager (about 15 KB), the newest snapshot, a copy of the engine's event ring, the
  engine's configuration and a scratch copy (about 18 KB each) and two blobs of the keypads' copy
  (`ALARM_CONFIG_BLOB_MAX`, about 32 KB each) on a master; on a keypad the manager, the blob being
  pulled, the decoded configuration and the UI's copies of it, the snapshot and 50 events.
  Smart Home on a master: the registry copy (`hc_device_t[64]`), two list blobs
  (`PLINK_DEV_BLOB_MAX`, 12.5 KB each) and two record sets (2.9 KB each), 8 device job slots and
  the 4 KB stack, about 55 KB, only when HOME is offered; with or without it, the keypads'
  published settings side (8 x `plink_keypad_home_t`), the settings queue (4) and the writes in
  flight.
  Internal RAM: about 260 bytes of statics (the published signal and caps, their console copy,
  plink_dev's state) and plink_dev's task control block; its locks and queue are PSRAM.
  Smart Home on a keypad: the core's mirror (`plink_dev_mirror_t`, about 31 KB) and the screen's
  `ui_dev_t[64]` (14.3 KB), PSRAM, allocated at the start only with home control and PSRAM, with
  their three locks; the settings side needs no memory of its own beyond small statics.
- One lwIP socket (UDP 6056, `SO_BROADCAST`); one mDNS service on a master, async queries on a keypad.
- NVS namespace `link` (stored formats: never change a key's meaning): `role` (u8,
  `plink_service_role_t`; absent = master), `master` (blob, `plink_store_master_encode`), `keypad`
  (blob, `plink_store_keypad_encode`), `master_ip` (u32, a keypad's master set by hand, network
  order), `otamaint` (u8 1: an update went in through the maintenance). It holds the pairing keys: never in a backup (`backup_policy.c` leaves it out), erased by a
  factory reset.
- One alarm listener (the tenth, `alarm_service.c` `ALARM_LISTENERS_USED`).
- Console (keypad): `plink dev [<i>]` (the mirror - list, rows, freshness, the last device data -
  and the screen's rows, one in full).
- Console (master): `plink dev [<i>]` (whether Smart Home is offered and which keypads have it, the
  list and records the master serves, one row in full), `plink prefs <n>` (keypad n's settings
  report) and, in development builds, `plink prefs <n> set <key> <value>` (Home Assistant's path,
  origin "console"; a delay in seconds, a switch on or off).

## Follow-ups
- Developer access on a keypad (`dev_remote`: network flashing, diagnostics): a keypad starts neither
  it nor the Panel Manager today (`panel_app` leaves `cfg->started` out).
- `net_wifi_hold_low_latency` has no unit test: `platform_esp` has no host build.
- `firmware/e32r40t/dependencies.lock` still has the manifest hash from before this component's
  `idf_component.yml` (CI doesn't build that board): its next build rewrites it.

## Rules
- Only the alarm task touches the engine: the link task never calls it and never waits for it; the
  worker does, and nothing else (LINK-027). Listener callbacks only hand over.
- The link task never takes `board_ui_lock` and never calls the UI: the UI reads copies.
- Codes are never logged or kept: every copy is wiped once handed over (LINK-018); keys never reach
  the log or the console.
- The role changes only by a factory reset (`plink role ... confirm`, or "Make it a stand-alone
  panel"), or by the setup's Done after a pairing.
- On a keypad, the update gate is the link's: no update while the link is down unless the keypad is
  not paired or in its no-code maintenance (LINK-025), or it hears its master but can't read its
  configuration (the master's newest status then decides, as when ONLINE); the update's self-test
  needs the link
  (`PANEL_OTA_OK_LINK`, beaten on authenticated frames and while not paired). An update let through
  by the maintenance is marked first (NVS `link`/`otamaint`); after its restart the link task beats
  for the self-test while it runs (socket bound, crypto proved), until the image is confirmed or the
  link is up again, then the mark goes.
