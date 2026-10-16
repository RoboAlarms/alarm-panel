# 09 · Wi-Fi, time and provisioning

**Status:** Implemented v1 - Wi-Fi through the C6, time from the internet with a time zone, the
first-time setup and Settings > Wi-Fi with a fixed address and the connection check; verified on
a panel (joined 2026-09-19, the check's seven steps 2026-09-20); the setup's role step and the
keypads' mDNS with the panel link (BL-048, tried on two panels 2026-10-16) · **Milestone:** M2, M4, M5 · **Depends on:** 06, 12, 14

**Code:** `components/services/platform_esp/src/net_wifi.c`, `net_check.c`;
`components/ui/alarm_ui/src/ui_net.c`, `ui_wizard.c`

## Goal
Get a new panel from power-on to protecting the house using only its own touchscreen: choose
the role, join Wi-Fi, set the time zone and codes. Then keep the network and clock healthy and
report problems as troubles.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| NET-001 | First-boot wizard on the touchscreen: language (English until the language step of docs/features/37 I18N-012, M22 G3), **role** (Master / Keypad), Wi-Fi (scan list + hidden SSID + on-screen keyboard), time zone, optional change of the factory installer (69420) and master (123456) codes (master role), done | preview (the role step and a keypad's pairing too, BL-048; on panels: M7), HIL |
| NET-002 | Wi-Fi uses the standard `esp_wifi` API through ESP-Hosted/`esp_wifi_remote` (C6 radio); WPA2/WPA3-Personal | HIL: joined a WPA2 network (2026-09-19) |
| NET-003 | Reconnect with backoff (1 s -> 60 s); raise the `network` trouble after 5 min disconnected (configurable), only when something depends on the network (MQTT or LAN keypads) | unit + HIL |
| NET-004 | SNTP time sync (default `pool.ntp.org`, configurable) with a POSIX TZ string chosen from a list; re-sync every 6 h | HIL: synced on the panel (2026-09-19) |
| NET-005 | Until the clock is set after boot - the first sync, or a battery-backed clock module that kept the time or was set by hand (docs/features/35) - the `clock_not_set` trouble is active and events are marked "time unknown". A module that kept the time sets the clock before the log starts, so the trouble is never raised | unit; docs/features/35 RTC-003 |
| NET-006 | mDNS hostname `roboalarms-<panel ID>.local` while a service is announced; `_roboalarms._tcp` while the Home Assistant integration is enabled (docs/features/23), `_roboalarms-dev._tcp` while developer access is on (docs/features/30), `_roboalarms-link._udp` for LAN keypads (M7). The names as built, decided 2026-09-28 | HIL |
| NET-007 | No HTTP/Telnet server on the panel by default (smallest attack surface; with the Panel Manager on, port 80 only redirects to its HTTPS, docs/features/31 WEB-023). The one planned listener is the Home Assistant integration's API: only while enabled, mutual TLS with pinned certificates after pairing (docs/features/23) | review |
| NET-008 | Wi-Fi credentials stored in the P4's NVS (encrypted in production builds), not on the C6 (`WIFI_STORAGE_RAM` there), so they survive C6 firmware updates | review |
| NET-009 | ESP-NOW and STA share the channel: the master's ESP-NOW channel follows its AP; channel changes are announced to keypads ([11](../11-Panel-Link-Master-Keypad/README.md)) | HIL |
| NET-010 | Settings > Wi-Fi: change Wi-Fi, see the signal (in words: strong, good, fair, weak; left out while the module reports none) and IP/MAC, forget network; opens with a master or installer code, outside installer mode, so a new router never needs the installer (docs/features/14) | preview (`24_settings_wifi.png`), HIL |
| NET-011 | The panel's address: from the router (DHCP, the default) or a fixed one (address, network mask, router), plus up to two name servers that win over the router's ones (also with DHCP). A cog on the connection row of Settings > Wi-Fi opens them; a mask may be typed as `255.255.255.0` or `24`. A fixed address is applied only when address, mask and router are all filled in and the router is on the panel's own network; one that can't be used is turned off at start-up, so a typo can never leave the panel unreachable. Same code as Wi-Fi itself (master or installer) | preview (`26_network_address.png`, `30_network_static.png`), HIL, console `ip` |
| NET-012 | "Check the connection" walks the network in the order the panel needs it - Wi-Fi, address, router, name lookup, internet, time server, connected service - and says what each step found or where it went wrong. It runs by itself after an address change, and from the button any time. Read-only: it changes nothing, and nothing it needs is required to protect the house | preview (`28_network_check_passed.png`, `31_network_check_failed.png`), HIL, console `netcheck` |

Notes on the table (2026-09-28):
- **NET-003, as built:** the phone notifications and the Home Assistant link depend on the
  network too, each with its own bit in `net_wifi_set_required()` (see "As built (M2)").
- **NET-006, as built:** the names differ from the requirement's. The hostname is
  `roboalarms-<panel ID>` (the panel ID comes from the MAC) and the Home Assistant integration's
  service is `_roboalarms._tcp` (docs/features/23 HAI-001), both from
  `components/services/ha_link/src/ha_link.c` `mdns_apply()`; remote development sets the same
  hostname and adds `_roboalarms-dev._tcp` (`components/services/dev_remote/src/dev_remote.c`,
  docs/features/30). The hostname is announced only while one of those services is on.
  Decided 2026-09-28: the requirement follows these names; the LAN keypads' service, which waits
  for the panel link (BL-048), is `_roboalarms-link._udp`.
- **NET-007:** the one listener it calls planned, the Home Assistant integration's API, is
  built (docs/features/23, M12). The development service of docs/features/30 is a further
  listener; like the Panel Manager it is off on a fresh panel until the installer turns it on.

## Design
- **Provisioning is local** (no phone app, no BLE provisioning): the 7" screen is enough, and it
  avoids an extra attack surface. The CYD's smaller screen uses the same wizard with a
  simplified keyboard (once its full UI fits; docs/features/19 is on hold, BL-079).
- **Role selection** writes `role=master|keypad` to NVS. A keypad jumps to pairing
  ([11](../11-Panel-Link-Master-Keypad/README.md)) after Wi-Fi. Wi-Fi is optional for a keypad
  that only uses ESP-NOW; it's still recommended so the channel matches and LAN fallback works.
- **Time**: SNTP through lwIP on the P4. Only the event log and schedules need wall-clock time;
  alarm logic uses monotonic time, so a missing clock never blocks protection.
- **Network state -> engine**: the platform calls `alarm_engine_system_trouble(ALARM_TRBL_NETWORK, ...)`
  and `(ALARM_TRBL_CLOCK_NOT_SET, ...)`.

### As built (M2: `components/services/platform_esp/src/net_wifi.c`)
- A `net` task starts Wi-Fi through the C6 and retries every 30 s with the `coprocessor` trouble
  while `esp_wifi_init()` fails. The C6 firmware version is logged and shown by `status`.
- Credentials, the TZ string and the time server live in the `net` NVS namespace. The console
  also sets them (`wifi <ssid> <password>`, `wifi scan`, `tz <POSIX TZ>`, `ntp <server>`).
  Default TZ: `UTC0`.
- Reconnect backoff 1 s doubling to 60 s. The `network` trouble follows NET-003:
  `net_wifi_set_required(<who>, true)` arms it, then 5 minutes without an IP raises it. Each
  feature that needs the network has its own bit (`NET_WIFI_REQ_MQTT`, `_NOTIFY`, `_HA`, `_LINK`
  for a master with keypads), so one switching off cannot clear another's need. Nothing
  required the network in M2, so it never fired then. A failed `esp_wifi_connect()` call
  schedules the same backoff retry (no disconnect event follows it), an unreadable network-trial
  record is cleared instead of aborting start-up, and the low-latency deadline for remote viewing
  is forgotten once passed (it wrapped after 24.9 days and switched power saving off for weeks;
  [B006](../../bugs/B006-notifications-lost-after-outage.md), 8367725; seeing its fix on a panel is
  BL-025).
- Wi-Fi power save is off while anything holds low latency (`net_wifi_hold_low_latency(owner,
  on)`, one hold per reason: remote viewing, the panel link's master with keypads and every
  keypad, docs/features/11 LINK-024); only the `net` task changes the radio's mode and restores
  the one it found when the last hold goes. A link keypad runs no time server while paired
  (`net_wifi_set_time_server(false)`): it follows its master's time.
- `clock_not_set` is raised at start when the clock reads before 2025 and cleared by the first
  SNTP sync (it doesn't beep by default). SNTP uses `pool.ntp.org` and re-syncs every 6 h
  (`CONFIG_LWIP_SNTP_UPDATE_DELAY`). Events carry `epoch_ms = 0` until then ("time unknown").

### As built (M4)
- **Scan**: `net_wifi_scan_start()` / `net_wifi_scan_results()` (strongest first, one entry per
  name, hidden networks left out). A connection attempt in progress is paused for the scan and
  resumed afterwards.
- **Why a connection failed**: `net_wifi_status_t.fail` (wrong password, network not found,
  other), shown as words on the screen. The status also carries the MAC address.
- **Time server**: `net_wifi_set_ntp_server()` (empty = `pool.ntp.org`), applied at once.
- **Screens** (`components/ui/alarm_ui/src/ui_net.c`): status (network, address, signal), networks
  nearby with signal and lock, password on the on-screen keyboard, "Other network..." for
  hidden ones, forget, look again. Used by Settings > Wi-Fi and by the setup.

### As built (M5)
- **Wi-Fi and the time are their own Settings sections** (Wi-Fi and network, Time and
  language), opened with a master
  (partition master too) or installer code. They used to sit in installer mode, which meant calling the
  installer after changing the router (docs/features/14, "Who can change what").
- MQTT turns `net_wifi_set_required(NET_WIFI_REQ_MQTT, true)` on while it is enabled, so a
  network down for 5 minutes becomes the `network` trouble (NET-003), next to `comm_fail` for
  the broker. The phone notifications keep their own bit while enabled; the Home Assistant link
  sets its bit when its listener starts and keeps it until the panel restarts
  (docs/features/26, /23).
- **Setup** (`ui_wizard.c`): runs when `sys/setup` isn't set (first boot, after a factory reset)
  or from Installer > Run the setup again.

### As built (addresses and the check)
- **Settings > Wi-Fi > the cog** (`ui_net.c`, `PAGE_NETWORK`, title "Network"): the address in
  use at the top, then how the panel gets it, the fixed address rows when it is fixed, the name
  servers, and the check. The cog is on the connection row and stays there while the panel is
  offline: a fixed address typed wrong is exactly when you need to get back in and undo it.
- **Filling one in**: each field is checked as it is typed (a dotted quad; the mask also takes
  `24`). While a fixed address is incomplete nothing is applied - the panel keeps asking the
  router and the page says what is still missing - so there is no half-applied state.
- **Storage and applying** (`net_wifi.c`): `ipmode`, `ipaddr`, `ipmask`, `ipgw`, `ipdns1`,
  `ipdns2` in the `net` NVS namespace. Only the net task touches `esp_netif`: a change notifies
  it, which stops/starts the DHCP client, sets the address and rejoins (rejoining is what makes
  esp_netif hand out the new address). DHCP overwrites the name servers when a lease arrives,
  so ours are re-applied after every address. With a fixed address and no name server set, the
  router is used - it nearly always forwards lookups, so a fixed address never silently loses
  them. A stored fixed address that doesn't pass its checks at start-up is turned off in NVS
  and logged (NET-011).
- **The check** (`net_check.c`, its own short-lived task, `net_check_get()` polled by the UI):
  a ping to the router, one `getaddrinfo`, a TCP connection to a public resolver on 443 (two of
  them, so one being blocked isn't read as "the internet is down"; no name lookup, which tells
  DNS trouble apart from a router that can't get out), one SNTP question to the time server,
  and a TCP connection to the connected service. Nothing about the panel is sent anywhere. The
  first step waits up to 12 s for the link, because the check most often runs right after an
  address change. Where the connected service lives comes in through
  `net_check_set_service()` (`components/app/panel_app/src/panel_app.c`, the start-up every
  board shares): the MQTT client builds on `platform_esp`, so `net_check` can't ask it
  directly. The Wi-Fi page is a general screen, so the step is called
  "Connected service", not by the integration's name (docs/features/07 UI-018). Each step's
  line is a pattern from `ui_backend/ui_check_words.h` ("{0} didn't answer") with its values
  (the network's name, an address): the console and the Panel Manager get it filled in in
  English, the touchscreen fills the same values into its own language (docs/features/37).
- **Not flashing**: the result rows are built once per run and only their words change
  afterwards, at most once a second, so the list never reflows while the check walks it
  (docs/features/07 UI-020, UI-023).
- **Console**: `ip` shows the setting and what is in use, `ip dhcp [<dns1> [<dns2>]]` and
  `ip static <ip> <mask> <gateway> [<dns1> [<dns2>]]` set it; `netcheck` runs the check and
  prints one line per step.

### Setup flow (master role)

```mermaid
flowchart LR
  A[Welcome] --> C[Wi-Fi, skippable] --> D[Time zone] --> E[Change factory codes? optional, each twice] --> G[System name] --> H[Done -> Home]
```

The role step (Master / Keypad, then pairing for a keypad) is added with M7, when the keypad
role exists; until then every panel is a master. Every step can be skipped and changed later
in Settings. An alarm during the setup takes the screen over; the setup continues afterwards.

### Battery-backed clock (2026-09-24, docs/features/35)
An optional DS3231 module on the I2C connector keeps the time through power cuts
(`components/services/panel_clock`). `net_wifi` counts the time-server syncs
(`net_wifi_time_syncs()`, the clock task writes the module after each) and lets the clock service
clear `clock_not_set` when the module or a person set the clock (`net_wifi_clock_set()`).
`status.time_synced` still means the time server. The first row of Settings > Time and language
says what the clock says and what set it last and, with a module, opens "Set the date and time"
(docs/features/35 RTC-008).

## Open questions
- Enterprise Wi-Fi (ESP-Hosted supports WPA-Enterprise): needed? Leaning no for v1.
- The internet step's targets (1.1.1.1, 9.9.9.9) are built in. Worth a setting for a household
  that blocks them, or is "Internet" simply reported as failed there? Leaning on leaving it.

## Tasks
- [x] ESP-Hosted Wi-Fi STA bring-up + reconnect policy (M2)
- [x] SNTP + TZ + clock trouble (M2)
- [x] Configurable NTP server (M4)
- [x] Setup wizard screens (M4)
- [x] The setup's role step (Master / Keypad, then pairing for a keypad) (NET-001): the screens
  (`ui_wizard.c`) and the firmware behind `ui_backend_t.link_*` (a panel in its setup pairs as
  a keypad, keeps its Wi-Fi and restarts as one) (BL-048, 18cc635, c7e4396, 32894bf)
- [x] Network settings screen: scan, password, hidden networks, forget (M4)
- [x] Fixed address, network mask, router and name servers behind the cog (NET-011)
- [x] "Check the connection", step by step, with what went wrong (NET-012)
- [x] The check on the real panel (2026-09-20): all seven steps pass (router 4 ms, 1.1.1.1 on
  443 in 117 ms, time server 49 ms and the clock right, the service skipped: no broker)
- [x] A fixed address end to end on the panel; the failure cases (wrong router, lost lease, dead
  name server, no internet, vanished access point) are asserted in `host/ui_sim` and the preview
- [x] mDNS while the Home Assistant integration or the development service is on: the hostname
  `roboalarms-<panel ID>`, `_roboalarms._tcp` (M12, docs/features/23) and `_roboalarms-dev._tcp`
  (docs/features/30); the names differ from NET-006's, see the notes under the table
  (`ha_link.c` `mdns_apply()`, `dev_remote.c` `maintenance()`)
- [x] mDNS: `_roboalarms-link._udp` on UDP 6056 while the master has a keypad or pairing is open,
  found by keypads (`plink_service`; LINK-021) (BL-048, c7e4396)
- [x] NET-006 follows the names as built, the hostname announced while a service is, the LAN
      keypads' service `_roboalarms-link._udp` (decided 2026-09-28; the row updated)
- [x] Joining Wi-Fi and SNTP on the real panel (2026-09-19)
- Duplicate of docs/features/06 (Hardware acceptance: "Wi-Fi reconnects after an access point
  reboot", BL-032): was "Reconnect after an access point restart (HIL)"
- [x] Signal strength through the C6: ESP-Hosted 3.0.9's host keeps the AP record's RSSI, which
  3.0.7 dropped; a bench panel reads -60 dBm (docs/features/06, findings 8) (BL-031, 2d4cde9)
