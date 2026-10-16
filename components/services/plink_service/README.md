# plink_service

**Tier:** services (ESP-IDF, board-agnostic) · **Features:** [11 Panel link: master and remote keypads](../../../docs/features/11-Panel-Link-Master-Keypad/README.md) (BL-048) ·
**Tests:** the protocol, pairing, sessions, both link managers, the stored records and the keypads' view are portable and host-tested in [`panel_link`](../../core/panel_link) (`LINK-NNN`); `host/link_sim` drives the same managers over UDP on a PC (LINK-013). This component has no host test of its own. On a panel: the `plink` console command; the HIL cases are docs/features/11 "Test and acceptance".

The panel link on a panel. Every panel has a role, stored once: the **master** (the alarm panel,
and every panel before this feature) or a **keypad** for a master elsewhere in the house. One task
owns a UDP socket on port 6056 and the role's link manager from `panel_link`, which does the
protocol; this component gives it the network, NVS, PSA, the engine and the screen.

- **On a master** the alarm listener hands the engine's news to the link task (a snapshot flag,
  events and one-shot sounds in queues that never wait); a worker task runs the keypads' commands,
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
| `include/plink_service.h` | `plink_service_role()`, `plink_service_force_master()` (a start that runs the alarm panel although a keypad role is stored); on a master `plink_service_listener()` (register before `alarm_service_start()`) and `plink_service_set_devices_port()` (Zigbee's numbers); `plink_service_start()`; on a keypad `plink_service_keypad_home()` (the UI's partition), `plink_service_keypad_power()` (the heartbeat's power flags) and `plink_service_update_idle()` (for `panel_ota_set_idle_gate`); `plink_service_extend_backend()` (the UI's `link_*` entries, and a keypad's link-served entries); `plink_service_console_register()` |
| `src/plink_internal.h` (private) | the link task's hand-overs and published copies, shared by the source files |

| Source | What it does |
|---|---|
| `plink_service.c` | the role, the link task, the socket, NVS `link`, mDNS start, the request hand-over |
| `plink_psa.c` | the crypto port (`plink_crypto_t`) on PSA: AES-128 ECB, X25519, the hardware RNG; proved by `plink_crypto_check` at every start; the cached AES key goes at the end of every link task turn (`plink_psa_forget`) |
| `plink_master_svc.c` | the master: listener, worker, the manager's port, mDNS, holds, the copies the UI reads |
| `plink_keypad_svc.c` | a keypad: discovery, commands, sounds, time, the update gate and heartbeat, the UI's copies; the setup's pairing (Option C) |
| `plink_backend.c` | the `ui_backend_t` entries |
| `plink_console.c` | the `plink` command |

## Depends on
- Project components: `panel_link` (the protocol and the managers), `platform_esp` (`alarm_service`, `net_wifi`, `panel_prefs`, `sounder`, `panel_log`), `panel_ota` (the heartbeat, the gate's answer), `ui_backend` (the UI's types), `alarm_mem`, `alarm_core`, `alarm_proto` (the panel id, `proto_fw_install_allowed`), `i18n`, and for the reporting state on a master `ha_link` and `net_mqtt`.
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
- PSRAM: the manager (about 15 KB), the newest snapshot, a copy of the engine's event ring, the
  engine's configuration and a scratch copy (about 18 KB each) and two blobs of the keypads' copy
  (`ALARM_CONFIG_BLOB_MAX`, about 32 KB each) on a master; on a keypad the manager, the blob being
  pulled, the decoded configuration and the UI's copies of it, the snapshot and 50 events.
- One lwIP socket (UDP 6056, `SO_BROADCAST`); one mDNS service on a master, async queries on a keypad.
- NVS namespace `link` (stored formats: never change a key's meaning): `role` (u8,
  `plink_service_role_t`; absent = master), `master` (blob, `plink_store_master_encode`), `keypad`
  (blob, `plink_store_keypad_encode`), `master_ip` (u32, a keypad's master set by hand, network
  order), `otamaint` (u8 1: an update went in through the maintenance). It holds the pairing keys: never in a backup (`backup_policy.c` leaves it out), erased by a
  factory reset.
- One alarm listener (the tenth, `alarm_service.c` `ALARM_LISTENERS_USED`).

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
