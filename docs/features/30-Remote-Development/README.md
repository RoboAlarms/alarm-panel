# 30 · Remote development, network flashing and screen control

**Status:** Implemented v1 - an enrolled workstation installs signed images over the network or
USB with probation and rollback, reads diagnostics and operates the screen; host-tested, on a
P4. The Z-Wave routes passed a no-sensor bench check; a sensor and the radio's
reset line remain (BL-019). Left: a second site over the VPN, touch takeover, fault, soak and
release-transition checks (BL-035). · **Milestone:** M17 · **Depends on:** 06, 07, 09, 13, 15,
21 (not Home Assistant)

**Code:** `components/services/dev_remote`, `tools/panelctl.py`,
`firmware/crowpanel_p4/main/dev_adapter.c` · **Original plan and research:** [DESIGN.md](DESIGN.md)

## Goal and scope

**Product evolution (implemented by Feature 31):** normal firmware now includes
panel-hosted HTTPS administration and separately gated developer resources. Persistent local
Installer switches control Web server / Developer mode. Release excludes the physical service
console, retains enrollment through OTA and supports authenticated development when enabled.
Network updates still require the current signed profile. Full-resolution JPEG, browser
configuration/upload and public-certificate enrollment live in `components/services/panel_web`; this
feature's Python tools remain optional. V1 left this service out of legacy and release builds;
[Feature 31](../31-Browser-Panel-Manager/README.md) superseded that.

Leave panels on the wall and develop over Wi-Fi or a routed site-to-site VPN. One person may
write the software while another does the electrical engineering at another site. Select an enrolled panel
by alias/IP or its attached USB port, install signed applications, inspect diagnostics, and
operate the real screen. A workstation can hold independent identities for multiple panels.

V1 delivers that workflow on the CrowPanel P4. It also introduces a display/input adapter and
board descriptor for future ports. New physical boards still need a build recipe, adapter and
hardware acceptance. The second site's 433 MHz receiver and optional USB bench computer were
explicitly deferred. The receiver driver and a console pulse capture now exist (docs/features/25,
`rf capture <ms>`); there is still no remote bench host or network RF capture, and
none is claimed here. The [original research and longer-term design](DESIGN.md) preserves those
plans and primary-source references. Current behaviour below supersedes proposed details there.

## Quick start

Run from the repository root with Python 3.10+ (the ESP-IDF venv works). Optional discovery
requires the pinned packages in `tools/panelctl-requirements.txt`:

```powershell
python -m pip install -r tools/panelctl-requirements.txt
pwsh tools/idf.ps1 -Profile dev build
# One-time bootstrap, only on the explicitly selected, disarmed, backed-up board:
pwsh tools/idf.ps1 -Profile dev -p COM4 flash
python tools/panelctl.py enroll --port COM4 --name workshop
python tools/panelctl.py info --device workshop
python tools/panelctl.py flash --device workshop --build --allow-older
python tools/panelctl.py screen --device workshop
python tools/panelctl.py diagnostics --device workshop
python tools/panelctl.py bundle --device workshop --output build/workshop-evidence.zip
python tools/panelctl.py logs --device workshop --follow
python tools/panelctl.py screenshot --device workshop --output build/workshop.png
```

`COM4` and `workshop` are examples, not universal device defaults. Read `PANEL.local.md` for
your enrolled local target. Use `-ChipRev v3` for v3 silicon at bootstrap. The tool
selects v1/v3 from authenticated device information for subsequent `flash --build` operations.
`--allow-older` explicitly permits an equal/older semantic version during development; it
never disables signature, board, silicon, profile, idle-state or capacity checks.

```powershell
# Explicit routed address, including a second site over a site-to-site VPN:
python tools/panelctl.py info --device bench-2 --host 198.51.100.20
python tools/panelctl.py flash --device bench-2 --host 198.51.100.20 --build --allow-older
python tools/panelctl.py screen --device bench-2 --host 198.51.100.20
# Find candidates; discovery never enrolls or flashes them:
python tools/panelctl.py devices
python tools/panelctl.py discover
python tools/panelctl.py discover --interface 192.0.2.10
python tools/panelctl.py discover --subnet 198.51.100.0/24
# USB selection verifies the enrolled ID/certificate without resetting the panel:
python tools/panelctl.py info --device workshop --usb COM4
python tools/panelctl.py flash --device workshop --usb COM4 --build --allow-older
# Query/rejoin an interrupted update's saved confirmation process:
python tools/panelctl.py update-status --device workshop
```

Target options work before or after the verb. With exactly one enrolled target, its alias may
be omitted. Multiple targets require unambiguous selection; no fleet operation or automatic
fallback from network to a reset-producing USB command exists. `--usb` without a port uses
the selected target's enrolled port. USB flashing wraps the existing signed serial OTA uploader
and verifies the returned ELF/boot/probation state; allow roughly eight minutes plus probation at 115200 baud.
An unenrolled legacy panel uses the explicit bootstrap procedure, not speculative serial probes.

mDNS is link-local and may not cross VLANs or the VPN. `--subnet` explicitly scans only TCP 6055
in a private IPv4 /24 or smaller with 12 workers and 400 ms connection timeouts. Results are
untrusted candidates. Use an enrolled alias with `--host` to authenticate a changed address.
No VPN changes, multicast reflector, public port forwarding or cloud service is needed.

```powershell
# Z-Wave (docs/features/36 Z9): status, live trace, network operations, the bench check.
python tools/panelctl.py zwave status --device workshop
python tools/panelctl.py zwave trace --device workshop --follow --output build/workshop.zwtrace
python tools/panelctl.py zwave add --device workshop
python tools/panelctl.py zwave allow --device workshop --keys access,auth
python tools/panelctl.py zwave pin --device workshop 12345
python tools/panelctl.py zwave check-in 3 4h --device workshop
python tools/panelctl.py zwave region us --device workshop
python tools/panelctl.py zwave check --device workshop --yes
python tools/panelctl.py zwave check --device workshop --yes --sensor
```

`zwave status` reads the radio, its devices and any running operation in words (`--json` for the
raw document). `zwave trace` follows the module's frame trace - a dev-build-only route - into a
`.zwtrace` file (`zwtrace 1`, a comment header with the tool and module versions but never the
home or panel ID, then one `<seq> <t_ms> <dir> <hex>` line per frame); by default it watches only
from where it starts (`--all` replays what the panel still has buffered), and it notes a `#
panel restarted` line when the module's sequence numbers restart. The network verbs (`add`,
`remove`, `stop`, `keep-listening`, `insecure`, `restart`, `dismiss`, `pulse-reset`, `allow`,
`pin` (5 digits, 00000-65535), `remove-failed`, `setup`, `ping`, `check-in`, `region`,
`reset-network`) print the panel's result
word and exit 1 when it isn't `ok`; `reset-network` also refuses locally without `--confirm`, and
commands that name a node look its current generation up from `zwave status` first (the panel
also accepts an absent generation, but the tool always sends the one it just read). `zwave check`
is the bench check of docs/features/36 hardware.md §7 in tool form - it needs only the ZAC93
(`--sensor` adds a guided pass for one device) - and asks for confirmation (or `--yes`) before it
restarts the radio and opens the add/remove windows; it writes its trace and a PASS/WARN/FAIL
report under `build/zwave-check-<date>/` (`--output-dir` for another folder) and exits 1 on any
FAIL. Each reset requires a newly captured, checksum-valid SerialAPIStarted followed by
Working status; stale Working status cannot pass a reset that did nothing. Add/remove must
reach the matching operation's Listening step, not merely Starting.

## Build and release boundary

Both IDF wrappers support `legacy` (existing default), `dev` and `release`, with separate build
and sdkconfig files. POSIX equivalents are `tools/idf.sh --profile dev [--chip-rev v3] build`
and `--dark`. Windows uses `-Profile dev`, `-ChipRev v3`, `-Dark`.

Since Feature 31 (ADR-033) every profile carries this service. It runs only while a
workstation is enrolled and the Installer's Developer switch is on (Installer > Remote access:
Web server and Developer, both off on a fresh panel).

| Profile | Remote development | Service console | Signed project name |
|---|---|---|---|
| legacy | compiled in; listens only when enrolled and switched on | existing configuration | alarm_crowpanel_p4 |
| dev | as legacy, plus the Z-Wave frame trace and the speaker switch | enabled | alarm_crowpanel_p4-dev |
| release | as legacy | disabled | alarm_crowpanel_p4-release |

The speaker switch (docs/features/08 AUD-025) is for a bench panel whose speaker may not be heard:
`GET /dev/v1/speaker` answers `on` or `off`; `POST` with the body `on` or `off` sets it and
answers the state after it (`panelctl speaker [on|off]`; the console's `speaker`; `info` has a
`speaker` field). Off keeps this panel's amplifier off, alarms included; paired keypads and
sirens still sound. It is kept in NVS across restarts and dev updates; a factory reset, or a USB
flash without `-Profile dev`, brings the sound back. A 500 means the change was not kept: its
text says whether the panel is silent now. Builds other than dev answer 404.

The voice pack (docs/features/08 AUD-027, voice-first-build.md D12) goes in over the same link, in
every profile, with the same certificate check:

| Route | Does |
|---|---|
| `PUT /dev/v1/voice` | the body is a whole signed `.robovoice` file (`application/octet-stream`, 8192..970752 bytes, a multiple of 4096; over the top is 413, any other size 400, both before the body is read). It is read into PSRAM and handed to `panel_voice_install`. Answers `{"result":"ok|unavailable|too_big|bad_signature|bad_pack|locked|busy|no_space|failed"}` with 200 (ok), 409 (locked, busy), 400 (too_big, bad_signature, bad_pack), 507 (no_space), 500 (failed), 503 (unavailable) |
| `GET /dev/v1/voice` | 200, the voice's state as JSON: `state` (`off`, `none`, `loading`, `ready`, `unavailable`), `draft`, `locale`, `voice_id`, `pack_revision`, `clips`, `file_bytes`, `why`, `enabled` (announcements on), `volume` (the Voice volume, 0-100), `word_gap_ms` (the gap between words in use now) and the counters (`utterances`, `completed`, `cancelled`, `underruns`, `decode_errors`) |

`panelctl voice install <file.robovoice> --device <name>` checks the file first (size, `RVOC`
header, payload length) and refuses a bad one without contacting the panel; it then prints the
result in words. `panelctl voice status [--json] --device <name>` shows the state. An install never
changes the Announcements switches or the speaker switch above.

The asset packs (docs/features/39 asset-packs.md "The panel", OPT-006: the letters, the saver's
picture, the certificates) go in the same way, beside the voice pack:

| Route | Does |
|---|---|
| `PUT /dev/v1/asset` | the body is a whole signed `.roboasset` file (8192..524288 bytes, a multiple of 4096; over the top is 413, any other size 400, both before the body is read; the kind comes from its header). It is read into PSRAM and handed to `asset_packs_install`: refused while armed or an alarm sounds, under the update machinery's claim, the signature, the envelope, the `RAST` format and the kind's rules, then written beside the working copy. Answers `{"result":"ok|off|size|signature|format|kind|draft|locked|busy|no_space|flash","kind":"letters|picture|certificates","state":"..."}` with 200 (ok), 409 (locked, busy), 413 (size: over the kind's bound), 400 (signature, format, kind, draft), 507 (no_space), 500 (flash), 503 (off). The letters and the picture load at once (`state` `ready`); the certificates say `needs_restart` |
| `GET /dev/v1/asset` | 200, `{"kinds":[...],"free_bytes":N}`: for letters, picture and certificates in that order `kind`, `name` (the store entry's), `state` (`none`, `ready`, `refused`, `damaged`, `needs_restart`, `unchecked`), `version`, `match` (hex), `bytes`, `draft`, `why`; `kinds` is empty without a pack store |

`panelctl asset install <file.roboasset> --device <name>` checks the file first (size, `RAST`
header, payload length, a kind 3-5 and its bound) and refuses a bad one without contacting the
panel; `panelctl asset status [--json] --device <name>` shows each kind's state.

Dev builds also have the audio bench (docs/features/08 AUD-030), for trying a voice's words and
joins on the panel without making a pack: `PUT /dev/v1/play` takes 16 kHz mono 16-bit samples
(little-endian, no header, up to 30 s; anything else is 413 before the body is read) and hands
them to `panel_voice_bench_play`, which plays them through the panel's speech path after what it
says now. Answers `{"result":"ok|unavailable|too_long|no_space","ms":N}`: 200 (ok), 503
(unavailable: no voice pack has loaded yet), 507 (no_space). `panelctl voice play <file.wav>`
checks the WAV's shape first; `tools/voice/voice.py try "Front Door" --device <name>` joins the
phrases from a recording's takes and plays them.

The voice bench (`tools/voice/bench_server.py`) has the panel say words from its own installed pack
instead. `POST /dev/v1/say` takes `<gap_ms> <id>...` as text: a gap of 0-250 ms (0 keeps the
pack's; any other stays until the next one, a restart or a new pack), then one to eight clip IDs
(decimal or 0x hex) of the installed pack, said as a Preview at the panel's own Voice volume
(`panel_voice_bench_say`). Answers `{"result":"ok|unavailable|bad_words|too_long|quiet",
"gap_ms":N,"volume":N}`: 200 (ok), 400 (a bad body, bad_words, too_long), 503 (unavailable,
quiet). `POST /dev/v1/voice` with the body `volume <0-100>` saves and applies the Voice volume as
the Settings slider does (the touchscreen's own copy follows) and answers as `GET` does; 400
`{"result":"bad_volume"}`, 500 `{"result":"failed"}` (not saved). `panelctl voice say` and `voice
volume` use them. Builds other than dev answer 404 (`say`) or 405 (`POST /dev/v1/voice`).

`CONFIG_ALARM_DEV_REMOTE` defaults on where there is PSRAM (`alarm_features/Kconfig`) and is also
set in the dev and release overlays. A release configuration fails at CMake
(`alarm_check_build_profile` in `cmake/alarm_firmware.cmake`) if the development flag,
crypto/HA fault injection or the service console is enabled; the release check below also
refuses the dark screen. The signed application descriptor carries board/profile identity. The
shared OTA writer accepts an exact project match, or an explicit same-board dev-to-release
transition; release rejects dev and unmarked legacy apps even with version forcing. Future
board names use the same suffix policy. Network uploads require the exact current project
(board and profile).

A dev-to-release transition ends network development builds: afterwards the panel's update
paths accept only release images. Use the explicit local signed serial/update procedure and verify
product health; never do this to a Wi-Fi-only panel without an independent
verification/recovery path. Enrollment and the Installer's switches survive the update
(`panel_ota.c` never erases workstation enrollment). Release-to-dev provisioning requires USB.
Hardened eFuse/encryption provisioning remains a separate future task requiring explicit
authorization (docs/features/15).

Check each release before packaging:

```powershell
python tools/check_release_profile.py --build firmware/crowpanel_p4/build_v1_release --config firmware/crowpanel_p4/sdkconfig.v1.release
```

It checks the release configuration (no development flag, service console, crypto/HA
diagnostics or dark screen), that the runtime access controls (`panel_web_start`,
`dev_remote_set_allowed`) are linked, the signed project name and that the Panel Manager is in
the image. Existing legacy firmware predates this policy and cannot bootstrap a differently named
app via its exact-project OTA check; use the one-time USB bootstrap. Keep the existing signing
key and flash layout. This feature never writes a bootloader, partition table, C6 or eFuse over
its network API.

## Trust and protocol

The panel generates its own persistent P-256 TLS identity in NVS `devrem`; each enrolled panel
has a separate workstation key/certificate in ignored `secrets/panelctl/`. USB exchanges public
certificates only. The PC pins the exact panel certificate before sending HTTP. The panel
requires TLS client authentication and checks the exact enrolled leaf fingerprint on **every**
request, including existing sessions and upload chunks. A certificate merely signed by the
trusted client certificate does not acquire authority. One enrolled workstation per panel is
supported in V1; enrolling a replacement invalidates the previous one.

Local serial commands: `dev status`, `dev describe`, `dev begin`, `dev chunk <public DER hex>`,
`dev commit`, `dev revoke`. Revocation disables access immediately and retries failed persistence
while running. If flash itself fails, power loss before successful persistence can restore old
trust: treat a non-OK revoke result as unfinished. The CLI saves new keys before committing
enrollment so a disconnect cannot lose the only key. An interrupted enrollment leaves a
`pending` registry record; inspect/restore it instead of silently generating new trust.
Enrollment over the network, from the Panel Manager's Developer page (`panelctl
prepare-enrollment` / `import-enrollment`), is Feature 31's: see its
[operator guide](../../../components/services/panel_web/web/README.md).

Listener: HTTPS port **6055**, prefix `/dev/v1`, protocol `api: 1`:

| Method/path | Contract |
|---|---|
| GET info | Stable ID, board/profile, silicon, version/ELF, random boot ID, slot capacity, health, OTA operation, dimensions/format/capabilities, display/memory/crypto metrics |
| GET logs | Bounded 32-record service/health log (sequence and device uptime); not unrestricted firmware printf output |
| GET diagnostics | Read-only partitions, source capabilities/link state, zone flags/report times/value, 16 recent public alarm events; excludes silent events and credential/configuration blobs |
| GET frame | Half-resolution RGB565 little-endian pixels; 503 while capture is busy (every screen is served: no private pages, see "Screen control and privacy") |
| POST control | `boot acquire` returns random controller token; `boot token input sequence x y pressed` or `boot token release` |
| PUT upload | Signed application bytes; required X-Image-SHA256 (64 hex), X-Operation (32 hex), optional X-Allow-Older: yes |
| POST install/cancel | Body is staged operation ID; install revalidates staged ELF and current safety state |
| POST confirm | Body `operation boot`; accepts only this pending candidate/boot |
| GET zwave | Z-Wave: the radio, the devices and the operation running (JSON, at most 32 KB); only when the firmware has Z-Wave |
| GET zwave/trace?since=N | The Z-Wave link's frames from sequence number N: at most 64 entries and about 12 KB; development builds with Z-Wave only |
| POST zwave | A Z-Wave request `{"op":...}` from the allowlist below, body at most 255 bytes: 200 `{"result":...}`; 400 for malformed JSON or an unknown op |

All routes require enrollment. No arbitrary console, GPIO writes, flash-address writes, shell,
raw NVS/configuration dump, factory reset or RF transmission endpoint exists. Diagnostics are
allowlisted fields, not a best-effort password scrubber. The Z-Wave routes keep to this: a
request is one of the operations the installer's Z-Wave pages and the `zwave` console command
already offer, refused by the panel's own rules, and no route sends a frame of the caller's
choosing (below). Add hardware-specific receive traces as bounded/versioned adapters when the
receiver is known; preserve timestamps and loss markers and export sanitized captures as host
decoder fixtures (DESIGN.md). The Z-Wave frame trace is the first such adapter.

V1 deliberately uses bounded HTTPS polling instead of the proposed WebSockets: one HTTP worker,
three TLS sockets, 4 KiB upload chunks, 5-second handshake/3-second receive timeouts, a five-minute
upload deadline and a reusable 307,200-byte P4 PSRAM capture buffer. The PC allows upload
backpressure for up to 330 seconds. Logs poll every two seconds; the viewer requests about one
frame per second. Three device connections are allowed (the listener purges the least recently used beyond
that; with two, an install's reply was lost while the viewer streamed, B016): close a separate
`logs --follow` session if a viewer and flash client already occupy them. Bundle exports include identity/health,
public diagnostics, the service log and a permitted screenshot, never private keys. It is a development console, not a high-frame-rate remote desktop.

## Z-Wave

For the bench with a real ZAC93 (docs/features/36 Z9): the panel runs its own Z-Wave firmware,
the workstation drives it and records what crosses the wire (decided).
Every P4 profile compiles Z-Wave in (`d51a3b0`, the P4's manifest
`sdkconfig.defaults`); the radio stays off, touching no pin, until `zwave place socket|j7` on
the console (or the Flasher's Connected parts) and a restart. `GET` and `POST /dev/v1/zwave` and
the `zwave` capability are there in every profile; the frame trace only in development builds,
so legacy and release answer 404 for it.

**The adapter.** `dev_remote_adapter_t` gained three optional callbacks, all NULL without
Z-Wave: `zwave_state`, `zwave_trace` (development builds only) and `zwave_request`. `dev_remote`
stays board- and feature-agnostic: it authenticates, bounds and routes, and knows no Z-Wave
word. The firmware's `main/dev_adapter.c` wires in `zwave_service`'s JSON under
`CONFIG_ALARM_ZSRC_ZWAVE` (the trace also needs `CONFIG_ALARM_BUILD_DEV`). With the callbacks
present, `info`'s `capabilities` ends with `"zwave"`. Buffers: the state 32 KB and a trace
12 KB, both from PSRAM per request; a request body 255 bytes, its answer 64.

**GET /dev/v1/zwave** -> 200, the touch pages' view of the network (`ui_zw_*`), as JSON:

```json
{"api":1,
 "radio":{"state":"working","place":"j7","firmware":"1.50","sdk":"7.22.1","chip":8,"region":"us",
          "home_id":"c0ffee01","node_id":1,"devices":2,"check_in_s":3600,"update_advised":false,
          "crypto":"passed"},
 "devices":[{"node":2,"generation":1,"model":"Zooz ZSE41 open/close sensor","security":"s2_access",
             "listening":false,"battery":90,"battery_low":false,"heard_s":120,"state":"ready",
             "unreachable":false,"check_in_s":3600,"check_in_next_s":0,"zones":[1]}],
 "progress":{"op":"add","step":"listening","seconds_left":52,"result":"none","node":0,
             "generation":0,"requested":["access","auth"],"dsk":""}}
```

| Field | Words |
|---|---|
| `radio.state` | `not_connected`, `starting`, `working`, `not_answering`, `unusable`, `foreign`, `replaced`, `updating` (its own firmware is being written, docs/features/36 Z11) |
| `radio.crypto` | `passed`, `failed`, `not_run` (the PSA self-test at start, run only with a place) |
| `radio.region` | `eu`, `us`, `anz`, `hk`, `in`, `il`, `ru`, `cn`, `us_lr`, `eu_lr`, `jp`, `kr` (serial-api.md §6.7's codes) |
| `devices[].security` | `unknown`, `none`, `s0`, `s2_unauth`, `s2_auth`, `s2_access`, `unconfirmed` (no key, its list not read since it joined), `none_capable` (lists S2, has no key) |
| `devices[].state` | `ready`, `setting_up`, `wake_needed`, `no_reports`, `not_usable`, `setup_stopped` (its interview gave up: `setup` continues it) |
| `devices[].power` | `unknown`, `battery`, `mains` (`listening` and `flirs` say how it hears, not its power) |
| `progress.op` | `none`, `add`, `remove`, `remove_failed`, `replace`, `ping`, `identify`, `routes`, `check_in`, `region`, `reset`, `restart` |
| `progress.step` | `idle`, `starting`, `listening`, `connecting`, `ask_allow`, `ask_pin`, `securing`, `reading`, `working`, `done` |
| `progress.result` | `none`, `added`, `added_insecure`, `downgraded`, `no_reports`, `nothing_joined`, `join_failed`, `already_added`, `controller`, `stopped`, `removed`, `removed_foreign`, `nothing_removed`, `still_answers`, `answered`, `no_answer`, `done`, `failed`, `setup_paused`, `timed_out`, `refused` |
| `progress.requested`, `keys` | a subset of `access`, `auth`, `unauth`, `s0` |

A device's `check_in_s` is its wake-up interval now (0: listening, or it never checks in) and
`check_in_next_s` one chosen for its next wake-up (0: nothing waiting); the change counts only
once the device confirmed it. An operation that ended reads `"step":"done"` with its result
until `dismiss` (an explicit `stop` gives `stopped`, a window that ran out `nothing_joined` /
`nothing_removed`).

What isn't known is 0, `""` or -1: `battery` -1 when unknown, `heard_s` -1 when never heard,
`firmware`, `sdk`, `region`, `home_id` and `place` `""`. The radio block is there even before a
place is chosen (`"state":"not_connected"`). `firmware` is major.minor with two digits
(`1.50`); `home_id` is eight lowercase hex digits; `zones` lists at most four. The words are
this contract: they change only by adding. 503 when the state can't be built now (memory).

**GET /dev/v1/zwave/trace?since=N** -> 200
`{"api":1,"next":M,"lost":L,"frames":[{"seq":12,"t":83412,"dir":"rx","hex":"0105..."}]}`.
`dir`: `rx` (a frame from the module, SOF to checksum), `tx` (a frame to it), `crx` / `ctx` (an
ACK, NAK or CAN received / sent), `text` (bytes outside frames: the bootloader's text, noise).
An `rx` whose checksum or length is wrong is a damaged frame (the panel's NAK follows) or one the
150 ms byte timeout cut off: the bytes as they came. `seq` counts from 1 at every start of the
panel, `t` is milliseconds since boot. At most 64 entries and about 12 KB per answer; `next` is
the number to ask for next - ask again at once when an answer came back full. A `since` older
than the ring starts at the oldest entry kept, `lost` counting what was skipped (and entries the
link could not record); no `since`, or 0, asks for everything kept; one past the newest (the
panel restarted since the last call) starts at the oldest too. 400 for a malformed `since`.

The ring (`zwave_service/src/zw_trace.c`, development builds): 1,024 entries and 16 KB of the
bytes they carried, 32 KB of PSRAM, the oldest overwritten first. The link task records every
byte it reads or writes; the HTTPS worker copies one entry at a time under the ring's mutex and
formats it after unlocking, so the link never waits longer than one small copy, and never more
than 5 ms: an entry that could not wait becomes a gap, counted in `lost`.

**POST /dev/v1/zwave** `{"op":...}` -> 200 `{"result":"ok|not_allowed|busy|no_radio|invalid|unsupported"}`:

| op | Takes | What it asks |
|---|---|---|
| `add`, `remove`, `stop`, `keep_listening` | - | listen 60 s for a device to add or remove; stop; 60 s more |
| `allow` | `"keys":[...]`; none: what the device asked for | the security question |
| `pin` | `"pin":"12345"` (a string of 5 digits) | the PIN on its label |
| `insecure` | - | add it without encryption |
| `remove_failed`, `ping` | `"node"`, `"generation"` (none: the node's now) | remove a failed device; check a listening one |
| `setup` | `"node"`, `"generation"` | continue a device's stopped setup (interview) where it stopped |
| `check_in` | `"node"` (0: new devices), `"generation"`, `"seconds"` (600..86400) | a check-in interval |
| `region` | `"region":"us"` | the radio's region (it restarts) |
| `restart` | - | restart the radio (its soft reset) |
| `reset_network` | `"confirm":true` | every device loses the panel: new keys, a new network |
| `dismiss` | - | the ended operation was seen |
| `pulse_reset` | - | pulse the module's reset line; `unsupported` when none is wired |

These map onto `ui_zw_request_t` (and the reset line's request) and go through `zwave_service`'s
one request slot, the console's, answered within 2 s. `allowed` is the console's: every
partition disarmed. Installer mode isn't asked for - the enrolled workstation is trusted as the
service console is; the touch pages still ask for it. The panel decides as for the console:
adding, removing, removing a failed device, region, reset, restart, check-in and the reset line
are `not_allowed` while a partition is armed; `stop`, `keep_listening`, `allow`, `pin`,
`insecure` and `dismiss` only answer an operation already running, and `ping` changes nothing. A
missing or out-of-range field, a word the panel doesn't know or `reset_network` without
`"confirm":true` is `invalid`, and nothing is sent. 400: not one JSON object (the strict reader),
`op` missing, not a string or not in the list, or a field named twice; members it doesn't know
are passed over. The log line names the operation and the result, never a PIN.

**What never crosses the link.** No route writes bytes of the caller's choosing to the radio.
The trace is wire bytes only, with any clear PIN or code blanked (`zw_redact.h`,
docs/features/36 ZW-030): what S2 protects stays encrypted there, and neither the network
keys nor S2's key-exchange secrets ever pass the wire (they stay in the panel's NVS and RAM). S0
would change that: its inclusion sends the network key under a fixed all-zero key. S0 is off
(`ALARM_ZWAVE_S0` is reserved and grants nothing), and a development build with it on refuses
to compile the trace until S0's key exchange is kept out of it. The state and the trace can show
node IDs, the home ID and the key printed on a device being added (its DSK): for the enrolled
workstation only.

## Update safety and recovery

Upload reuses `panel_ota` begin/write/end and its one-writer exclusion, header/hash/RSA signature
checks, inactive app slot and alarm-task reboot gates. Network staging and install are separate.
If the alarm becomes unsafe before selection, the network install cancels rather than waiting
indefinitely to reboot later. Interrupted uploads leave the running app untouched; re-upload
is explicit, not an automatic mutation retry. A SHA-256/ELF/operation record is saved beside the
local device registry for resuming outcome checks.

Before boot selection, the panel persists the candidate ELF and operation in `devota`. During
candidate probation, normal engine/UI/sensor/touch/log heartbeats **and** authenticated network
reconnect confirmation are required. Minimum healthy probation is 60 seconds; no confirmation
by 180 seconds causes rollback. The PC checks the new boot ID, expected ELF and operation before
confirming, then waits until probation actually ends before reporting success. Losing the client
or network conservatively rolls back. Keep old firmware's Wi-Fi/NVS schema readable.

This does not recover every possible hung firmware, flash/NVS corruption or broken power/network
condition. The optional on-site USB bench host remains the independent recovery option; hardware
and setup were deferred. Enroll over USB before a panel is mounted where only the network
reaches it. Since Feature 31 a workstation can also be enrolled later from the Panel Manager,
once its Web server and Developer switches are on (the Web server switch is turned on at the
panel itself).

## Screen control and privacy

`panelctl screen` serves a browser UI only on random-port `127.0.0.1`. A random session token
arrives in the URL fragment and is removed from the address bar. The bridge requires exact Host,
Origin and session headers, uses no-store/CSP headers and passes no device private key to the
browser. It forwards only info/frame/control; local loopback access alone grants no authority.

The viewer starts view-only. Take control obtains an exclusive two-second lease renewed by
pointer traffic/heartbeat. Sequence numbers reject replay, boot IDs invalidate old inputs,
coordinates use the advertised full display dimensions, and queued mouse movement is bounded.
Physical touch takes priority in the board adapter; the LVGL input driver releases on timeout
without relying on network progress. It refuses to inherit a local authenticated/installer
session; normal code and lockout rules continue to apply, and lease cleanup returns
authenticated/installer flows Home so the panel is never left unlocked. **No private pages
(decided):** every screen, including Wi-Fi, users, codes and integrations,
can be viewed and operated remotely. The viewer is already signed in with a master/installer
code (or an enrolled mTLS workstation) and panel settings still ask for their own code. The
earlier rule blocked those pages, which ejected a remote user entering Home Assistant setup.
One exception (BL-131 finding 7, LOG-008): Activity's private events. A takeover waits while a
code shows them at the panel, and the private step a remote session opened is forgotten when
its lease ends, like an authenticated flow.
Do not treat remote developer trust as an alarm disarm code.

P4 capture reads the composed display buffer under the existing UI lock; network writes occur
outside it. Busy capture returns 503 (the 423 "private" answer no longer exists; viewers
still treat one from older firmware as paused).
The viewer leaves the last frame steady (no whole-screen opacity flashing) and disables input
when the frame becomes stale. Display ISR/cache-safe configuration and flicker guard remain unchanged. Capture,
virtual input and metrics live behind `dev_remote_adapter_t`; the browser reads dimensions at
runtime. Future boards need their own safe capture/storage strategy, not P4 display-pin logic.

## Verification and remaining acceptance

Evidence lives in ignored `build/dev-*` logs. Host tests, simulator/preview assertions and
hardware tests are separate claims. Never run the `hardware_*.py` scripts automatically in CI.

Tests: `tools/tests/test_panelctl.py` (DEV-003/004/006/007/014) and
`tools/tests/test_panelctl_zwave.py` (the `zwave` commands against a fake panel), both run by CI
(`host-tests.yml`); `components/core/alarm_proto/test/test_fw.c` (the signed-profile rule,
DEV-004/015); the preview's DEV-010 checks in `host/ui_preview/main.c`; on an enrolled panel only,
`tools/tests/hardware_auth.py`, `hardware_load.py`, `hardware_remote.py` and `hardware_tamper.py`.

V1 acceptance on a P4, 2026-09-21 (the counts are that day's):

- Host: 435 cases / 26 suites pass, including signed profile boundaries and a second board-name
  fixture. Python client: 15 tests pass (identity, pinning, selection, image parsing, reconnect,
  rollback outcome, bounded scan and USB wrong-device refusal). CI runs these Python tests.
- UI preview: all 112 screens pass, including refusal to inherit installer authority. A real
  browser viewer navigated Home -> Zones -> Home and released control on the local P4.
- Hardware: signed network upload/reboot/authenticated reconnect/health probation passed.
  A different candidate with confirmation deliberately withheld rolled back to the known-good
  image and restored remote access without USB intervention.
- Hardware auth: missing client cert, TLS-valid but unenrolled derived cert, competing
  controller, wrong lease, replay and expired lease rejected. Existing-session revocation and
  restoring the original enrollment passed.
- Combined dark capture/flash stress: 58 frames received in 45 seconds, five optional captures
  skipped while UI busy; about 56 display fps, zero late frames/guard trips, heap integrity PASS,
  crypto ready with zero faults. Lowest sampled internal memory 202,755 bytes; console stress
  sample about 190 KiB. The optional `crypto_test` fault-injection command is excluded from the
  dev configuration: no claim of 100 on-device hash injections is made. Normal deployment
  passed: disarmed/READY, backlight 55%, 55.9 fps, no late frames/guard trips, heap PASS and crypto
  faults zero. No one was asked to look at the physical screen.
- Framebuffer-gate acceptance: 25 captures / 30 seconds plus a browser viewer, five busy skips,
  lowest sampled internal free memory 170,755 bytes; heap integrity PASS and crypto
  ready/faults zero. Flash stress remained about 56 fps with no late frames/guard trips. Browser
  reload retained its session. Live bridge tests rejected missing session, hostile Origin and
  hostile Host.
- Unified USB flash installed a normal dev image, verified a changed boot/expected ELF and
  completed health probation; final delivery uses network OTA.
- Local PC and panel are on different routed subnets. Explicit IP, USB identity and bounded
  subnet discovery passed. mDNS enumeration returned no candidates across that route; multicast
  advertisement is implemented but same-LAN discovery remains untested. A signed network update
  reached a second panel over a site-to-site VPN (docs/features/31 "Test and acceptance"); the second-site
  acceptance is still owed (Tasks).
- A tampered network app with a recomputed transport hash was rejected at image/signature
  validation without changing the running app.
- Release builds for v1/v3 compile and `check_release_profile.py` (configuration, runtime access
  controls, signed project name, Panel Manager present) passes; these checks are required before
  packaging. An isolated contaminated release configuration was rejected during CMake
  configuration. Neither release nor v3 firmware is flashed to this v1 panel.
- Z-Wave on the link (built only): P4 v1 dev with Z-Wave compiled in is 2,625,536 B signed,
  55.6 % of the slot; against the same build without the link's part, +7,166 B flash, +33,024 B
  PSRAM (the trace ring) and +72 B internal RAM. P4 v1 release 2,363,392 B (50.1 %), +1,680 B
  flash and +24 B internal RAM (the route handlers and the adapter's three pointers, never used
  there). No new warnings under `-Wextra -Wconversion -Wshadow -Wformat-truncation=2`. The ring
  (eviction by slots and by bytes, gaps, `since`/`next`/`lost`, the 64-entry and 12 KB bounds),
  the state JSON (escaping, the worst case of 29.6 KB) and the request parser (every op, 32
  invalid and 25 refused bodies) were run on the PC against stubs of the RTOS.
- Z-Wave on the bench (recorded in docs/features/36 and
  [B013](../../bugs/B013-zwave-own-node-suc-callback.md)): `panelctl zwave check` ran against a
  ZAC93 on a panel; the software restart and the add/remove windows pass, the reset
  line produced no SerialAPIStarted (wiring or module revision to check); no sensor added yet.

What still needs hardware or time is in Tasks below. Touchscreen enrollment, several workstation
identities, crash/boot-log export, an independent bench host and receiver-specific RF
capture/replay are future work there, not hidden prerequisites for the delivered Wi-Fi path.

## Tasks

Each open box names its row in [the backlog](../../BACKLOG.md). What V1 delivered is under
"Verification and remaining acceptance"; the checks on panels can only be ticked from a panel.
[DESIGN.md](DESIGN.md) "Phases and tasks" is the original plan, not this list.

**On panels**
- [ ] A second site's panel over the site-to-site VPN: enrolled with its own identity, selected
  by alias with `--host` (`info`, `flash --build`, `screen`, `update-status`), and two enrolled
  panels never confused (DEV-003, DEV-017). A signed update already reached a second panel over
  the VPN on 2026-09-24 (docs/features/31 "Test and acceptance") (BL-035)
- [ ] Discovery on the panel's own LAN: `panelctl discover` finds it over mDNS (so far only an
  explicit address, USB and `--subnet` scanning across a routed subnet have been tried) (BL-035)
- [ ] Taking over from a physical touch: a touch at the panel ends the remote lease, releases
  every press and returns authenticated or installer flows Home (DEV-010) (BL-035)
- [ ] Network and power-loss faults: power cut during an upload; the access point or the VPN lost
  during transfer and probation; the client restarted before confirming. Each ends in a bounded,
  queryable outcome, with no duplicate install (DEV-006, DEV-007) (BL-035)
- [ ] A 24-hour soak with a developer session open (DESIGN.md "Test and acceptance"); it can
  share BL-032's 24-hour platform soak run (BL-035)
- [ ] A dev-to-release transition on a spare board: the local signed serial update, product
  health checked, development images refused afterwards on every update path (DEV-015)
  (BL-035)
- [x] Enroll a panel's replacement board explicitly under a new alias over USB,
  never replacing the pinned certificate (BL-018)
- [ ] The Z-Wave routes with a sensor: `zwave check --sensor`, a device added with S2 through
  `zwave add` / `allow` / `pin` while `zwave trace` records it, and the reset line
  ([B013](../../bugs/B013-zwave-own-node-suc-callback.md)) (BL-019)

**Build and release**
- [ ] The release profile built for v1 and v3 in CI with `check_release_profile.py` run on it
  (DESIGN.md "Test and acceptance", build matrix; CI's `firmware.yml` builds only the legacy
  profile today) (BL-050)

**Tracked elsewhere**
- Duplicate of docs/features/15 (the `hardened` profile and its provisioning, BL-049): hardened
  eFuse and flash-encryption provisioning, never over this feature's network API.

**Later** (BL-091)
- [ ] A second board type with its own adapter and build recipe, accepted on hardware (DEV-012).
  The 4" E32R40T has no developer link (`CONFIG_ALARM_DEV_REMOTE` off, no PSRAM) (BL-091)
- [ ] Enrollment from the touchscreen (DEV-016), and more than one workstation per panel (BL-091)
- [ ] Crash and boot-log export, after a privacy review of what a dump holds (BL-091)
- [ ] An independent on-site USB bench host reached over the VPN: device leases, boot logs and
  recovery with the panel's network down (DEV-018) (BL-091)
- [ ] Receiver-specific RF capture and replay over the link as a bounded, versioned adapter
  (DEV-019); `rf capture` on the console exists (docs/features/25) (BL-091)
