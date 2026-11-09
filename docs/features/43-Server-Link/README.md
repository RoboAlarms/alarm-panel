# 43 · The link to alarm-server, direct mode and the app's push

**Status:** Built and tested on two bench panels (the 7" and the 10.1"), from the service console and
through a bench alarm-server. The touchscreen pages (Settings > Integrations > RoboAlarms, and Add a
phone on a person's page) are built and walked in the PC preview; on the 10.1" bench panel's own screen the linked
page (status and the ceiling's switches) and Add a phone's QR code are tried, the code and Allow steps not yet.
In the Panel Manager, Integrations > RoboAlarms shows the link's state and the remote ceiling and
offers Unlink (with the password), and a person's page lists their phones with Remove; linking, Allow,
the ceiling's switches and adding a phone stay at the panel (BL-225). **Backlog:** BL-225.

The plan and its reasons are alarm-server's `docs/features/02-Panel-and-App-API` (`panel-link.md`,
`direct-mode.md`, `push-api.md`); requirement ids `API-`, `DIR-` and `PUSH-` are from there. This page
says what the panel does and what it doesn't yet.

## What it does
- **Link**: MQTT 5 over `wss://<server>/mqtt`, outbound. An Ed25519 key made once (NVS `srvlink`, never
  exported, not in backups); each connect signs a counter that only goes up. Not linked and not
  linking, it holds no connection.
- **Linking**: a code from an Owner or Admin opens it; the server gives a link code; a person claims it;
  the panel names them and waits for Allow. Unlinking works with no server.
- **Events**: every engine event, with `at_ms`, `boot`, `uptime_ms` and what set the clock; the log is
  the queue, and only the server's `down/ack` moves the position. A first link starts from now.
- **Countdown**: each partition's status and delta carry `delay_left_ms` beside `delay_remaining_s`,
  and `uptime_ms`, so the app counts an entry or exit delay down to the millisecond the panel sent.
- **Clock**: each connection measures the offset; no clock, or more than 2 s off, is set from the server.
  A time answer long after the hello it echoes is not used.
- **Commands**: through the remote ceiling, then the engine as that panel user. Never queued; the same id
  gets the same answer for 10 minutes.
- **Tunnelled requests**: `panel_web_tunnel()` runs the Panel Manager's own handlers and access table as
  panel user N. Refused whatever the table says: `frame`, `screen`, `control`, `login`, `logout`,
  `password`, `remote`, `factory-reset`, `developer*`.
- **Direct mode**: phones paired by QR code on the panel's HTTPS API, signing in with their own key;
  disarm, silence, people and Setup each need a fresh signature the panel checks.
- **App push**: per-phone AES-256-GCM to the relay, signed RFC 9421, using `notify`'s words and limits.
- **From the security review** (2026-11-02): a sealed code is `rs1|time|action|area|code`, taken only
  for that command, within 2 minutes of the panel's clock, and once; only a disarm-class action takes
  one (`clear` with a code used to skip its state check). The tunnel refuses `POST partition`
  (commands carry the disarm ceiling and the code) and a people or Setup change without `fresh`.
  A phone goes with its user on every config change, linked or not; a removed phone's session ends at
  once; direct push reaches only phones whose user has the event's partition.
- **Setup mode** (`POST setup-mode {open}`): Setup mode as the person, held by the server's source
  until it closes it or the 15-minute idle timeout ends it; Setup page changes apply inside it. The
  system master (the Owner) may open it, and the installer while the Owner lets the Admin code
  open Setup mode (CORE-PROG-014), as at a keypad (anyone else: 403 `installer_only`, the name
  kept); the remote ceiling's `remote_setup` switch and the step-up
  still apply, and setting the installer or master code (`installer-code`) is behind them too; 409 `not_disarmed` or
  `setup_mode_in_use`. A Setup page's flat fields are wrapped for the Panel Manager's handlers
  (`values`, or `changes` with the current revision). The ceiling message carries the Owner's
  "Let the Admin code open Setup mode" as `installer_setup` (added 2026-11-08; absent from older
  panels, read as true), sent again whenever it changes: the website and the app show it
  read-only, since only the Owner changes it, at the panel.
- **People codes from the app** (MOB-044): `POST people {action:"create"|"set_code", ..., sealed_code}`;
  the code is sealed to this panel's X25519 key as `rs1|time|set_code|<user>|code` (or `create|0`),
  opened only here, for that slot, within 2 minutes and once (400 `bad_code` otherwise). The seal key
  is signed with the link key in the hello (`seal_sig`, F3).
- **Automations from afar** (F18): over the server, a rule that disarms, unlocks, opens or stops is
  saved switched off (`approve_at_panel: true`) and only turned on or tried at the panel, unless the
  remote ceiling lets that person disarm from afar without a code anyway.
- **Read-only for the app**: `GET outputs` (`{outputs:[{number, kind, name, on, trouble}]}`, the wired
  outputs by channel and the 433 MHz sirens) and `GET backup` (`{last_at, by, by_name, size}` of the
  last backup made; never the file), the system master's without Setup mode.
- **Found on the home network**: while the Panel Manager is on, mDNS `_roboalarms-app._tcp` on 443 with TXT `id`
  (the id the pairing QR code carries), `name` and, when linked, `srv` (the server's base URL), on the shared
  responder, all the time as HomeKit and Matter devices do; no firmware version or model (the owner, 2026-11-06). It grants nothing: pairing still needs the code shown at the panel (DIR-001). `web` on
  the console shows it.
- **Keypads**: the link and the phones are the master's; a keypad's touchscreen doesn't offer them.
- **Panel Manager**: `GET server-link` (state, server, location, `heard_s_ago`, the ceiling) and
  `POST server-link {action:"unlink"}` (system master, password again, never through the tunnel);
  `GET people` carries each person's `phones` (`id`, `name`), `POST people {action:"phone-remove",
  number, phone}` removes one (the person or whoever manages them).

## Service console
`server` prints the state and `server help` the rest: `host`, `ca`, `link <code>`, `allow`, `deny`,
`cancel`, `unlink <code>`, `ceiling <code> ...`, `relay`, `offline <s>` (a bench's outage), `phone ...`.

## Not built yet
- Setting the remote ceiling from the server with an Owner's step-up (panel only today).
- F23: server technicians (time-limited Admins) have no panel user today. If one ever gets one, the
  automations' per-run check must enforce the maker's own arm, disarm and device rights.
- Panic through the server: the `command` message (`proto_srv_command_map`) takes no `panic` yet, so a
  phone raises one only directly (docs/features/31 WEB-032). The same `opt.app_panic_mask` would gate it.
- The server path for files, emergency contacts on the alarm screen (`contacts` is ignored), cellular.
- The app-push catch-up summary after an outage (NOTE-036): it sends one by one, within the gate's limits.
- Per-phone level choice on a page, `browser` in `people` (always false), battery and region in
  `snapshot`, IANA time zone (the panel stores POSIX; alarm-server maps it).

## Findings on the bench
- **A flood of connections** (docs/bugs/B033, SEC-012): see the bug report; one TLS gate now stands in
  front of every listener, and the Wi-Fi co-processor has a watchdog.
- **Internal RAM**: a TLS handshake takes about 50 KB of internal RAM for a moment. With Developer mode on
  as well as a connected link, it reached the point where Wi-Fi traffic stalls for 4-5 s (radio RPCs time
  out, Panel Manager requests drop). With Developer mode off: free 90 KB, no stalls. Release builds don't
  have Developer mode.
- **Image size**: see BL-225 for the numbers: libsodium's Ed25519 table is about 96 KB.
