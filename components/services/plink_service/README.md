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
  LINK-019) and tells the engine about silent keypads (`alarm_service_device_reachable_at`, with the keypad's home partition, LINK-007;
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
- **A keypad's calls of the master's pages** (docs/features/11 "The master's settings and
  installer menu on keypads", BL-223; `plink_mrb_svc.c`, `plink_krb_svc.c`). On a master, each
  keypad's remote-backend call runs on the worker with `ui_remote_serve` over this panel's own
  backend (`ui_port_esp`'s, as the Panel Manager takes it), installer mode, users and codes acting
  as `{ALARM_SRC_KEYPAD, n}`; a code accepted at the keypad's code check is its grant; the grants
  go to the keypads (RB_GRANT) every second when they change, and `link_info` names a keypad
  holding installer mode. On a keypad with a screen, the screen's entries for the master's pages
  are `ui_remote` proxies: a write is handed to the link task and waited for from the screen's call
  task; a read answers from a copy the link task keeps fresh while the screen reads it, and
  `link_info.remote_gen` moves when one changes. All their buffers are PSRAM (about 300 KB on a
  master, 270 KB on a keypad), allocated once; without them `PLINK_CAP_RB` isn't offered.
- **On a keypad** (`panel_app` starts no engine: `alarm_service_set_dormant()`) the link task
  finds the master (mDNS, the subnet broadcast, or an address set at the console; again whenever
  the manager's handshake goes unanswered, `port.resolve`; the subnet broadcast, ip | ~netmask,
  is named as one to `plink_keypad_master_addr` and `plink_keypad_pair_start`, since its bytes
  don't say so on a subnet other than a /24), and pairs by that broadcast when mDNS finds no master
  open for pairing within 10 s (`PAIR_BROADCAST`; the first master's answer is taken), keeps the
  copies the touch UI reads, sends its commands (answered within 3 s, refused at once while the
  link is down, panic excepted; the screen hands them, its code checks, device actions and
  partition switches to `plink_call`, a task of their own, through `ui_backend_t.run_later`, so it
  never waits for its master: LINK-054), plays the master's sounds, follows its clock and time zone (no
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
- **Updates through the link** (docs/features/11 "Updates through the link", BL-048): keypads
  follow their master and nobody is asked anything. Both sides offer `PLINK_CAP_FW`.
  - On a master (`plink_fwm_svc.c`): each link task turn sets STATUS's UPDATING (its own run or
    its self-test) and, while keypads are paired, LINK-041's question
    (`panel_ota_set_keypads_check`: the keypads on line when its restart came, NVS `link/kup`).
    Once a second (every 100 ms while its restart waits for the INSTALLs' echoes) the planner
    (`plink_fw_plan`) steps on a world made of panel_ota - the target is its own update waiting in
    its spare slot (`panel_ota_spare_image`), else the version it runs; INFO names it only once
    confirmed; the server, channel and key from its trust record; its image from the spare or
    running slot; the copy for another model (`panel_ota_fetch_for`, at most every 5 s,
    `panel_ota_copy_free`) - and the core (`plink_master_fw_keypad`); its orders go to
    `plink_master_fw_order`, the Activity stages 9-12 (`plink_fw_log_step`, once each) to the
    worker, which posts them, and the rows are published (`plink_service_fw`, MQTT's
    `.../firmware` `keypads`, `plink fw`). Its own restart (`panel_ota_set_restart_with`) is held
    while a keypad on line still downloads or receives the target (at most 30 min from when its
    own update was ready: they download alongside it and restart with it, decision 7) and while a
    keypad installs alone or is on its self-test (`plink_master_fw_busy`); once its gate passes,
    INSTALL goes to every READY keypad and the echoes are waited for (5 s at most). The relay's
    bytes come through `port.fw_read`: the spare or running slot (`panel_ota_spare_read`,
    `panel_ota_running_read`: flash windows, the link task's internal-RAM stack) or the copy in
    PSRAM.
  - On a keypad (`plink_fwk_svc.c`): its master's version is kept in NVS `link/mver` and applied as
    panel_ota's cap whenever the core's seq moves (and at the start, before the first INFO); no
    scheduled check of its own while the session has FW (`panel_ota_set_checks(false)`). FETCH
    and RELAY run through `panel_ota_order_fetch`/`_relay` (the order's server and key become its
    trust record), the relay's bytes through `panel_ota_write` from the link task (`port.fw_write`)
    and `panel_ota_end` once the last is in; INSTALL once the core holds it for the READY version
    and the gate passes twice a second apart (`plink_keypad_fw_hold`, and no fire or CO tone kept
    through a lost link: `plink_keypad_fw_gate` KEPT_TONE), then `panel_ota_order_install`. After
    an install its master ordered it is on trial (`plink_keypad_fw_trial`), and panel_ota's
    self-test asks the trial (`panel_ota_set_trial_check`). Its continuous tone takes fire and CO
    from the STATUS flags too (`plink_keypad_continuous`). `plink_service_fw_keypad` gives the
    screens its run (state, target, percent, paused, the banner, on trial) and its master's name
    and version.
  - On a PC: `host/link_sim` runs both sides with a stand-in for panel_ota (`link_fw.c`), and
    `tools/link_fw_sim.py` runs the scenarios.
- **The setup** pairs through a keypad manager on the same socket while the master service
  still runs (Option C). Before LINK-053 provisioning it persists the paired record and keypad
  role, then starts an authenticated session. Done completes setup and restarts as a keypad.
  Before that durable transition, leaving cancels the provisional pairing. After it, Wi-Fi
  failure or manual selection retains enrollment, and an interrupted setup resumes on reboot.
- **Enrollment Wi-Fi** uses PLINK_CAP_WIFI and sealed WIFI_REQ/WIFI. Credentials come from
  platform_esp under its network configuration lock, excluding pending trials; the keypad only
  installs into an empty configuration. It retries every second for 30 seconds, cancels before
  manual entry, wipes secrets and reports progress/online state through `ui_link_pair_t`.
  Language, display and audio are unaffected. Radio discovery scans channels without Wi-Fi.

## Public API
| Header | What it offers |
|---|---|
| `include/plink_service.h` | `plink_service_role()`, `plink_service_force_master()` (a start that runs the alarm panel although a keypad role is stored); on a master `plink_service_listener()` (register before `alarm_service_start()`), `plink_service_set_devices_port()` (Zigbee's numbers), `plink_service_set_home()` (the registry runs: Smart Home offered), `plink_service_keypads()` and `plink_service_prefs_set()` (the keypads' settings for Home Assistant), `plink_service_fw()` and `plink_service_fw_row_name()` (each keypad's update); `plink_service_start()`; on a keypad `plink_service_fw_keypad()` (its updates from its master), `plink_service_keypad_home()` (the UI's partition), `plink_service_keypad_power()` (the heartbeat's power flags) and `plink_service_update_idle()` (for `panel_ota_set_idle_gate`); `plink_service_extend_backend()` (the UI's `link_*` entries, and a keypad's link-served entries); `plink_service_console_register()` |
| `src/plink_internal.h` (private) | the link task's hand-overs and published copies, shared by the source files |

| Source | What it does |
|---|---|
| `plink_service.c` | the role, the link task, the socket, NVS `link`, mDNS start, the request hand-over |
| `plink_psa.c` | the crypto port (`plink_crypto_t`) on PSA: AES-128 ECB, X25519, the hardware RNG; proved by `plink_crypto_check` at every start; the cached AES key goes at the end of every link task turn (`plink_psa_forget`) |
| `plink_master_svc.c` | the master: listener, worker, the manager's port, mDNS, holds, the copies the UI reads |
| `plink_home_svc.c` | the master's Smart Home and settings for its keypads: the `plink_dev` task, its builds and device actions, the settings writes, Home Assistant's keypads list |
| `plink_fwm_svc.c` | the master's keypad updates: the planner's world and step, its orders, the copy, `fw_read`, UPDATING, its restart's hold and INSTALLs, LINK-041's question and `link/kup`, the Activity events, the published rows, `plink fw` |
| `plink_fwk_svc.c` | a keypad's updates: `link/mver` and the cap, the orders through panel_ota, the relay's writer, the gate and INSTALL, the trial, the published run, `plink fw` |
| `plink_keypad_svc.c` | a keypad: discovery, commands, sounds, time, the update gate and heartbeat, the UI's copies, Smart Home's mirror and the screen's devices and actions, its settings for the master; the setup's pairing (Option C) |
| `plink_backend.c` | the `ui_backend_t` entries |
| `plink_console.c` | the `plink` command |

## Depends on
- Project components: `panel_link` (the protocol and the managers), `platform_esp` (`alarm_service`, `net_wifi`, `panel_prefs`, `sounder`, `panel_log`), `panel_ota` (the heartbeat, the gate's answer), `ui_backend` (the UI's types), `alarm_mem`, `alarm_core`, `alarm_proto` (the panel id, `proto_fw_install_allowed`; public for `proto_link_keypad_t` and `proto_prefs`), `device_service` (the registry and the judge, on a master), `panel_voice` (a keypad's voice state in its settings report), `i18n`, `ui_remote` and `ui_port_esp` (the master's pages for keypads, BL-223), and for the reporting state on a master `ha_link` and `net_mqtt`.
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
  Updates through the link: on a master the planner's world, step and log, panel_ota's status,
  image and copy facts, the published rows and the event queue (16), PSRAM, allocated at the
  start; on a keypad its run's state and the published copy, PSRAM; internal RAM: the few bytes
  panel_ota's hooks read (the planned target and its hold, the trial).
  Smart Home on a keypad: the core's mirror (`plink_dev_mirror_t`, about 31 KB) and the screen's
  `ui_dev_t[64]` (14.3 KB), PSRAM, allocated at the start only with home control and PSRAM, with
  their three locks; the settings side needs no memory of its own beyond small statics.
- One lwIP socket (UDP 6056, `SO_BROADCAST`); one mDNS service on a master, async queries on a keypad.
- NVS namespace `link` (stored formats: never change a key's meaning): `role` (u8,
  `plink_service_role_t`; absent = master), `master` (blob, `plink_store_master_encode`), `keypad`
  (blob, `plink_store_keypad_encode`), `master_ip` (u32, a keypad's master set by hand, network
  order), `otamaint` (u8 1: an update went in through the maintenance), on a keypad `mver` (blob:
  schema u8 1, the pairing's net id u16 little-endian, its master's version as len u8 and bytes;
  erased when none is known) and on a master `kup` (u32, bit d-1: the keypads on line when its own
  restart into an update came, erased once it runs confirmed). It holds the pairing keys: never in a backup (`backup_policy.c` leaves it out), erased by a
  factory reset.
- One alarm listener (the tenth, `alarm_service.c` `ALARM_LISTENERS_USED`).
- Console (keypad): `plink dev [<i>]` (the mirror - list, rows, freshness, the last device data -
  and the screen's rows, one in full), `plink fw` (its run, its master's version and the cap).
- Console (master): `plink fw` (keypad updates: the target, its image, each keypad's row, run and
  last order), `plink fw update <n>` (ask keypad n again now: a wait, a refusal or a going back
  forgotten); `plink dev [<i>]` (whether Smart Home is offered and which keypads have it, the
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

## ESP-NOW alongside LAN (BL-048)

`plink_radio.c` runs Hosted peer-data RPC on a separate worker, with bounded PSRAM
queues and a 200 ms transmit-queue age limit. Its callback only validates and copies.
A factory C6 without the bridge leaves LAN working; INIT repeats to recover after a
radio reset. Peers are restored on demand by the C6 bridge.

A live session sends small sealed frames over both paths. The original ciphertext
and sequence number are reused, so the existing authenticated replay window accepts
only one copy and a command runs once. Each side replies to the source of the newest
authenticated frame. ESP-NOW uses 250-byte frames; a large LAN page is retried at the
smaller MTU after a heartbeat moves the peer. Radio replies also probe the subnet by
UDP, allowing LAN to return without pairing again. Plain handshake frames use only
the selected discovery path.

Master beacons advertise channel and pairing availability every 100 ms. A keypad
without a fresh STATUS for 11 s asks its network task for a 5 s radio scan window,
then cycles channels 1–13. Beacons are only hints; they never establish freshness or
move a live peer. The paired key must authenticate a handshake or a sealed frame.
Wi-Fi reconnect attempts continue after the window. Supervision remains active when
the radio bridge works even if the access point is down.

`plink radio` reports bridge readiness and channel. Development builds also accept
`plink radio window` temporarily disconnects Wi-Fi for five seconds without changing saved
credentials, for development-only radio fallback checks.
`plink drop lan|radio|off`: a RAM-only, 90-second fault injection covering transmit and
receive for that transport, to test automatic recovery without changing credentials.
These operations do not enable either panel's speaker.

Development builds also provide `plink wifi` (enrollment Wi-Fi state and authenticated
connection), `plink wifi auto` (the setup's automatic request/retry), and `plink wifi manual`
(the Select Wi-Fi cancellation). These use the same service requests as the touchscreen;
they neither accept nor print credentials and cannot bypass paired authentication.
States are 0 idle/manual, 1 requesting, 2 joining, 3 connected, and 4 unavailable.

Partition policy (KPD-006): Installer > Keypads and devices sets each panel's default,
allow switching/locked, idle seconds (30 by default), and Never below the seconds selector.
A locked panel has no Home switcher; otherwise it appears below the left card when multiple
partitions are enabled. Choosing needs no code; alarm actions still do. Input anywhere, including
Activity and remote typing, renews the inactivity timer. Temporary views never save preferences.
The master enforces remote policy and persists it in each peer record; its own policy is local
NVS. Audio preferences remain separate.

Bench commands: `plink policy <device> allow|lock <seconds|never>` on the master;
`plink cmd goto <partition>` on a keypad. `plink` shows saved home and current selection.
Reconnection, reassignment and policy changes clear temporary selection, including with Never.

Panel Manager also uses the published pairing/keypad copies and management queue on headless masters. These calls retain installer-mode and disarmed checks. Browser confirmations carry the displayed code for a second check on the link task, avoiding a stale published snapshot authorizing a replacement comparison.
