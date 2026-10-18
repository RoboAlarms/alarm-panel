# 31 · Browser Panel Manager

**Status:** Implemented v1 - the panel serves its own Panel Manager over HTTPS: configuration
under its access table, sign-ins, signed uploads, the remote keypad, and an Overview that is the
alarm command centre (arm and disarm as the signed-in person, without a code, WEB-024; the installer never arms or disarms, USR-001) and a Software updates page that takes updates from the firmware server, follows a run with Stop and the restart, and gives the installer the server and the channel (BL-186, BL-188; docs/features/13). Host- and
mock-tested, on a P4 since 2026-09-22; Left: the remote keypad rework on a panel (BL-080), a
second site over the VPN (BL-035), the Overview's arming on a panel (BL-113), the redesigned zone
page, People and codes, Troubles and sign-in without codes on a panel (BL-138), event-log sign-ins
and passkeys (BL-064), helper accounts (BL-076). · **Milestone:** M18 · **Depends on:** 03, 07,
09, 13, 14, 15, 21, 30

The approved [keypad management and enrollment flow](keypad-enrollment.md) supports normal and
headless masters (BL-048). Its service and browser tests pass; live enrollment acceptance remains.

## Objective and delivered scope

The panel hosts its own browser interface for household configuration, full-resolution remote
keypad assistance and signed firmware updates over LAN or the existing site-to-site VPN.
No Python bridge, Node server, cloud account, Home Assistant, external assets or SD card is
required to operate it. JavaScript executes in the browser; HTTPS, state, configuration,
capture and update verification run on the panel. Building source still requires ESP-IDF.

Both Web server and Developer mode ship in normal firmware, with separate persistent
Installer switches. Fresh installations default off; previously enrolled Feature 30 panels
migrate enabled. Turning web off turns both off, closes listeners and revokes sessions.
Turning developer off retains normal browser administration. Re-enabling web requires local
Installer access. Release excludes the physical service console and diagnostic commands,
but retains the runtime-gated services and enrolled workstation trust. Network images must
still match the current signed board/profile identity. No eFuse or partition migration.

## Operator entry points

See [the operator guide](../../../components/services/panel_web/web/README.md) for first connection, certificate
verification, network recovery and Wi-Fi-only workstation enrollment.

- Local **Settings > Installer** shows the web URL and certificate SHA256 fingerprint.
- Open the panel's HTTPS address over LAN/VPN and verify that identity before trusting the
  per-device self-signed certificate. Browser trust has not been silently installed or bypassed.
- Sign in with a user name and a browser password set on the panel's screen ([Sign-in](#sign-in-2026-09-23)),
  or one set by whoever manages you in the Panel Manager's People page (temporary until you
  change it, unless they chose that you keep it, WEB-017); keypad codes no longer sign in. No default credentials: nobody signs in
  until a password is set. An already enrolled mTLS workstation can obtain a one-use, 60-second
  browser grant.
- **Remote keypad** shows the panel's screen (H.264, or JPEG) over one WebSocket; being on the
  page is control (no Take control), a touch at the panel wins for 3 s, and the
  panel shows a red banner the person there can pause it with. Every panel screen can be viewed
  and operated (no private pages; docs/features/30).
- **Developer** supports signed application upload, expected-image reconnect/probation,
  bounded diagnostic logs and public-certificate workstation enrollment while enabled.
- Optional `panelctl` supports explicit aliases/IPs, USB recovery and bounded network discovery.
  It can hold multiple independently pinned panels. Browser access uses each panel's URL;
  a webpage does not scan arbitrary VPN subnets or provide a central fleet service.

## Requirements

WEB-001..WEB-015 are the first design's requirements and acceptance gates, kept in
[DESIGN.md](DESIGN.md) "Requirements and acceptance gates". WEB-016 onwards came with the
rework after it.

| ID | Requirement | Verified by |
|---|---|---|
| WEB-016 | The Panel Manager allows what the panel's own table (docs/features/14) allows, per API path, fail closed; nothing changes while armed or in alarm; what everyone may read shows only what the panel shows them (their partitions, no update server). No request carries a keypad code: changes go as the signed-in person (docs/features/01 CORE-USR-027, CORE-USR-028, CORE-PROG-011), and the person's password again, unless typed in the last 10 minutes, guards remote access, enrolment, a backup's download and restore and factory reset ("Recent password") | `panel_pages.test.cjs` (four roles, refusals, locked, partitions and update source) against the mock, which mirrors the table; code review of `panel_web.c` |
| WEB-017 | Users in the browser as at the panel: the installer manages everyone, a master all but the installer; sign-ins for the installer, masters and standard users; a password set for somebody else is temporary unless the person setting it chooses that they keep it (one you set for yourself never is) | `test_users.c` CORE-USR-012/025, `panel_pages.test.cjs` "who manages whom", preview `119` |
| WEB-018 | A remote tap is pressed and released on the panel: network delay can't make a key repeat | `panel_frames.test.cjs` "a quick click is one tap"; the panel side not yet on hardware |
| WEB-019 | Being on the remote keypad is control; the panel shows a red banner and the person there can pause it, allow it or turn remote access off | preview `120`, `121`; `panel_frames.test.cjs` |
| WEB-020 | The screen streams as H.264 when the browser decodes it, JPEG otherwise or when chosen on the page, only when it changed, with a low-delay SPS | `test_h264_sps.c`; `panel_frames.test.cjs` "still pictures when chosen"; the mock's real H.264 decoded in the Browser pane; not yet on hardware |
| WEB-021 | Typing on the computer's keyboard reaches the panel's text sheet or code pad | preview `120`, `panel_frames.test.cjs`, the Browser pane against the mock |
| WEB-022 | **Set the date and time** (docs/features/35 RTC-008): on a panel that can keep it (a clock module answers), a card above the time zone says what the panel's clock reads and lets a master or the installer set it from this computer's clock or a typed date and time (this computer's time zone, UTC on the wire, `POST time {epoch_ms}`). The hardware is never named (review feedback: "Saying Clock Module on the WebUI makes no sense"); the overview's clock tile says what set the clock in plain words | `panel_pages.test.cjs` ("the date and time are set by hand only where the panel can keep them"), the Browser pane against `MOCK_CLOCK=ok` |
| WEB-023 | **Plain HTTP goes to HTTPS** (review feedback: "The panel does not respond on port 80. When the web server is running it should cause a redirect to the https"): while the Panel Manager runs, port 80 answers every GET and HEAD with `301 Moved Permanently` to the same path over HTTPS, and serves nothing else (no page, API or cookie in the clear; other methods are refused). The Location keeps the address the person typed only when the Host is plainly a host name, IPv4 or bracketed IPv6 (the port dropped), else the panel's own address: no header splitting, no redirect elsewhere. `Cache-Control: no-store`, because home-network addresses are handed to other devices later. No HSTS: the certificate is the panel's own, and HSTS would stop a browser letting the person accept it. Stopped with the HTTPS server (Web server off) | `components/core/http_redirect/test` (host), console `web` (`HTTP80=redirect`), `curl -I http://<panel>/` on hardware |
| WEB-024 | **Arm and disarm as the signed-in person** (BL-113): `POST partition {number, action, level, no_entry_delay, silent_exit, bypass_open, zone, life24}` runs `arm` (Away, Stay, Night), `disarm`, `clear` (code + OFF while disarmed: clears the alarm memory), `acknowledge` (OFF alone: acknowledges trouble beeps and silences a fire, CO or auxiliary siren), `chime`, `bypass`/`unbypass` and the walk test's `walk_test_start` (`life24`: *Include panic and 24-hour zones*), `walk_test_end` and `walk_test_extend` (docs/features/40 WALK-001, WALK-002, WALK-025) as that user, with no code: the sign-in proved who it is, and the engine applies that user's own code rules (`alarm_engine_command_as()`, CORE-USR-027: authority, partitions, duress, states). Each action runs only in the state it is for: `arm`, `clear`, `chime`, `bypass` and `unbypass` while the partition is disarmed and installer mode is closed, `disarm` while it is not disarmed (exit time, armed, entry time, or an alarm sounding while disarmed), `acknowledge` in any state, the three walk test actions while it is disarmed and installer mode is closed, and the engine runs each only in the state it is for (a start while one runs, an end or an extend while none: `bad_state`); otherwise `409 {"error": "bad_state", "zone": 0}` and nothing runs. A walk test's start and extend are a system or partition master's or the installer's, refused by the server's table before the engine (`403 not_allowed`); its start is locked while a partition is armed or in alarm, its extend is not; its end is anyone's who may disarm the partition, as the engine decides, and works wherever a disarm does. The state is checked on the alarm task in the same call that runs the command (`alarm_service_command_as()`'s `when`), never from an earlier snapshot, so a partition a keypad, MQTT or a keyfob arms meanwhile is never disarmed by "Clear alarm". The event names the person and "Panel Manager" (source `remote` id 31). `403` for a partition the viewer doesn't see, and for an enrolled workstation's grant session (`password_sign_in_required`: only a person's own password stands in for a code), `400` for a bad body, `503 Memory busy` when the panel has no memory to check the request with, `409 {error, zone}` with the engine's reason (`not_ready`, `not_authorized`, `bad_state`, `not_bypassable`, `held`, `disabled`) and `zone` the first zone that stopped an arm, else 0. Disarm and acknowledge are allowed while armed or in alarm: disarming is how the lock ends. For the Overview, each `status` partition adds `level`, `no_entry_delay`, `silent_exit`, `armed_by`, `armed_by_name`, `delay_left_ms` (less the snapshot's age), `exit_delay_s`, `entry_delay_s`, `ready_levels`, `chime`, `alarm` (any alarm active now: burglary or panic while armed, kept after the siren timed out until the disarm, and fire, CO or auxiliary in any state, disarmed included), `alarm_kind` (`fire`, `co`, `burglary`, `panic`, `aux`, the most urgent first, or `""`), `memory` (alarm memory, only while disarmed with no alarm active), `siren`, `canceled`, `bypassed`, `trouble_unack` and, for a walk test running on it wherever it was started (WALK-024), `walk_test`, `walk_tested`, `walk_total`, `walk_remaining_s`, `walk_started_by`, `walk_started_by_name` and `walk_started_from` (zero and blank while none runs); each `zone-status` zone adds `levels`, `always`, `entry_s` and `bypassable` (the engine's own zone rules). The Troubles page (BL-137, WORD-008) lists what the touchscreen lists, from `GET troubles` (the access table below), grouped by what each does to protection, and `status`'s `trouble_count` (active plus restored) counts it for the menu and the Overview; the page reloads the list only when that count changes. Its Acknowledge does what code + OFF does at the touchscreen: `clear` for each disarmed partition with no alarm memory and no walk test that has something to acknowledge, `acknowledge` (the beeps only) for the others, never a partition in alarm | `test_users.c` CORE-USR-027; `panel_pages.test.cjs` (arming by role, not ready and bypass, guest and installer disarm, disarm while locked, the Overview's fields, alarm memory, the walk test's nine) against the mock; firmware flags syntax check of `panel_web.c`; code review of `panel_web.c`; not yet on hardware |
| WEB-026 | **Mockup first**: a new or changed page, section, field, button, flow or wording in the Panel Manager starts as a proposal in [the Panel Manager's mockup](mockup/README.md) - the real pages and their pretend panel running in one page, shown Today, Proposed or Both - and is built only once the person you work for has approved it; built, the proposal file goes. Bugs need none. The touchscreen's rule is docs/features/07 UI-044 | `manager_mockup.test.cjs` (the mockup builds; every page, role and proposal renders against the pretend panel in the page); the approval recorded in the proposal |
| WEB-025 | **Languages on this panel** (docs/features/37 [language-packs.md](../37-Globalization/language-packs.md), I18N-046): `GET languages` (anyone signed in) lists the installed language packs with their state (ready and how many phrases show in English, made for another version, can't be used here, damaged), the store's free space, what the update source offers and the last download; `PUT languages` (the body is a `.robolang` file) installs one (it goes with `PUT voices`: a language and its voice are chosen from a list and downloaded together, ADR-053, BL-206), `POST languages {action: remove|download, locale}` removes or fetches one - a system or partition master or the installer, never while armed or in alarm; every answer carries `result`, which the page puts into words. `/i18n/<locale>.json`'s ETag carries the pack's SHA-256, so a new pack reaches the pages without a firmware change. The card sits on Display and sound | `panel_pages.test.cjs` (six I18N-046 tests) against the mock; firmware built; not yet on hardware (BL-121) |

## Browser = panel (WEB-016)

Every API path has one entry in `panel_web.c`'s table ("who may use what"), checked before its
handler runs; anything not listed is the installer's (fail closed). The levels are docs/features/14's:

| Level | Who | Paths (GET / POST) |
|---|---|---|
| anyone | any signed-in user (at the panel: no code) | `status` (the viewer's partitions; `trouble_count` is the length of the touchscreen's Troubles list, active and restored, and `trouble_sig` its fingerprint, which moves when a line comes, goes or is restored: the page reloads `troubles` when it differs from that answer's `sig`), `history`, `zone-status` (the viewer's partitions, the panel's words; each zone's read-only `spoken` {words, summary, state: empty, ready, off, missing, unavailable or review, binding_rev, approved_rev, pack_rev}, computed and never taken by a write, docs/features/08 voice-first-build.md D14), `troubles` (BL-137: `{troubles:[{family: system|keypad|zone, kind, number, restored, since_ms}], sig}`, the touchscreen's list from `alarm_snapshot_troubles()` over the viewer's partitions; `kind` is the alarm's system or zone trouble number, 240 for a keypad; `since_ms` the epoch of its newest event among the 256 newest log entries, 0 when not found or the clock was unknown), `time`, `storage`, `config/display` (a change of its language or region: shared; turning `voice_enabled` on is refused with `no_pack` or `unavailable` until the voice is ready, D06), `languages` (the list; changes: shared), `voices` (the voice pack and its readiness: `{installed, ready, locale, name, version, draft, reason, free}`, 404 in a firmware without the voice; installing: system), the remote keypad (`frame`, `control`, `screen`), `partition` (arm, disarm, bypass, chime, a walk test's end as yourself: the engine decides by your authority and partitions, WEB-024), `logout`, `password` (your own) |
| users | installer, system or partition master | `people` (the users you see; changes as yourself, no code, never by a workstation's grant: the engine applies your authority, CORE-USR-028) |
| shared | the same | `walk-test` (the walk test page's list) and `partition`'s `walk_test_start` and `walk_test_extend` (docs/features/40), `config/display` changes with `lang` or `region`, `languages` (install a pack, remove, download), `config/network`, `wifi`, `wifi-scan`, `network-check`, `time` changes |
| system | system master, installer | integrations (`config/mqtt|ntfy|pushover`, `home_assistant`, notification test/status, ntfy topic, certificate authorities), SD card actions, `PUT voices` (the body is a signed `.robovoice`, 8192 to 970752 bytes in whole 4096-byte blocks, else `too_big` before it is read; never while armed or in alarm: `armed`; every answer is `{result}`: `ok`, `too_big`, `bad_signature`, `armed`, `busy`, `not_allowed`, or the language upload's `no_space`, `flash`, `off`; installing never changes the Announcements switches, AUD-029; the body is received and installed by one upload task of its own, never the HTTPS worker, so arming, disarming and the Remote keypad answer during a slow upload, and the sign-in is checked again before installing), `updates` (the status, check, install and stop; the update server and the channel are the installer's), `automations` and `automations/devices` (docs/features/41: the rules, the pickers and their edits; a POST as yourself, never by a workstation's grant, and not while armed); a `config/mqtt` change to `allow_panic` is the installer's (BL-107) |
| installer | installer | `alarm` (as the signed-in installer, CORE-PROG-011), `sources`, `binding/N`, `zone-sources`, sensor search/listen, `installer-code`, `factory-reset` (+ a recent password), `backup` (+ a recent password), `remote` (+ a recent password), developer logs, enrolment (+ a recent password), signed upload |

- **Recent password** (OWASP ASVS 5.0 V7.5; decided 2026-10-02): before what lets a computer in
  for good, carries every secret away or can't be undone - `remote` (the Web server and
  Developer switches), `developer/enroll`, `POST backup` (download), `PUT backup` (restore) and
  `factory-reset` - the person's own password again, unless they typed it in this session in
  the last 10 minutes (a password sign-in counts; a workstation's grant never does). The
  request carries it as `me_password` (a restore: header `X-Panel-Password`, the password's
  UTF-8 bytes in hex, like `X-Backup-Password`). Missing: `403 password_required`; wrong:
  `403 wrong_password`, counted by the sign-in gate (`429` while it is shut); right: the next 10
  minutes ask no more, and a try waits 2 s after the last one, a sign-in's included. `status`
  carries `reauth_s`, the seconds left (0: the next one asks). People and codes and the
  installer and master codes ask for nothing: the role decides. Your own browser password
  changes only on Your password (`POST password`, the current one first): `people` refuses
  `web-password` and `web-off` for yourself, or a session left open could choose a password
  its owner doesn't know and pass every check above. Evidence: code review and the bench (the
  check is ESP-IDF code, outside the host tests).
- **Locked while armed** (`alarm_snapshot_locked()`, the touchscreen's UI-022 rule, one copy):
  every change but the remote keypad, `partition` (disarming ends the lock; the engine itself
  refuses what a state forbids; a walk test's start is locked, its extend and end are not), your
  own password and signing out answers `409 locked`.
- `GET walk-test` (docs/features/40 WALK-017, WALK-018): `{seq, trips, partitions: [...]}` for the
  viewer's partitions, each `{number, name, running, remaining_s, started_by, started_by_name,
  started_from, started_s_ago, seq, tested, total, life24, ended_by, ended_by_name, end_how, trips,
  zones, live}` - the test running or the last one's summary until the next start or a restart.
  `started_from` is `panel`, `panel_manager`, `keypad`, `console`, `remote` or `zone`; `end_how`
  `command`, `disarm`, `time_limit` or `""`. `zones` are those the test counts, each `{number,
  name, tested, cover, how (fault|cover|button|sensor_test|""), first_s, trips, open, check}`;
  `live` the ones that stay live (fire, CO, 24-hour unless included), `{number, name}`. `seq` and
  `trips` are the sums over these partitions (a new mark raises `seq`, every trip `trips`), and
  `key` a digest of everything the page shows but the time left (marks, trips, a start, an end,
  an extend, a zone opening, closing or getting a CHECK, the restart): `?key=<key>` unchanged
  answers only `{unchanged: true, key, seq, trips, partitions: [{number, running, remaining_s}]}`.
- `status` carries `access` (users / shared / system / installer) and `locked`; the browser's
  menu shows only what the table allows and a notice while armed. Masters no longer read
  installer programming ("read-only installer pages" are gone); the Zones page for everyone else
  is the panel's Zones view: name, sensor kind and the panel's own words (`notify_zone_state()`,
  NOTE-006), refreshed every 3 s.

## Users in the browser (WEB-017)

- **Who manages whom** is the engine's rule, `alarm_config_user_manages()` (CORE-USR-025): the
  installer everyone (deliberate choice 14 in docs/features/01), the system master everyone but the
  installer, a partition master itself and the low users of its partitions.
- **Browser sign-ins** for the installer, system and partition masters and standard users (not
  arm-only, guest or duress: `web_login_eligible()`). People and codes: `POST people {number,
  action, ...}` as the signed-in person, no code (`alarm_engine_user_*_as`, CORE-USR-028: the
  actor's slot read again, their authority applied; refused for a workstation's grant,
  `403 password_sign_in_required`): a save `{authority, partitions, name, new_code}` (no
  `new_code`: name, role and partitions only), `delete`, `web-password {password, temporary}`
  and `web-off`. `temporary` (missing: true) makes a password set for somebody else
  `REC_TEMPORARY` (changed at their first sign-in); `false` lets them keep it; one you set for
  yourself never is. At the panel one set for somebody else is always temporary. `GET people`
  answers `code_min`, the fewest digits a new code may have. Reconcile drops a sign-in whose
  user becomes arm-only, guest or duress.
- People and codes (BL-136) lists who you see as rows: name with "You", user number, role (and
  partitions when there are several) and sign-in state (Browser sign-in / Waiting for their own
  password); a row opens the person's page (`#/people/N`, a chevron only for people you manage).
  The page follows the touchscreen's user page: name and partitions, What this code can do as
  choices with the touchscreen's lines, their code (its length from `code_min`), Panel Manager
  (browser sign-in; "Set a new password" at `#/people/N/password`, with "Ask them to change it
  when they sign in", off unless turned on), and Remove this person apart. Changes wait in a bar
  at the bottom (what changed, Undo, Save) and Save sends them with no review and no password;
  only Remove asks first, and a refusal (a code another person has) shows in the bar. Your own
  page has no switch and no password box: "Change your password" opens Your password, with one
  new-password box. An eye on every password and code box shows what was typed until pressed
  again, and hides it when the form is sent, so there are no Repeat boxes. "Add a person"
  opens `#/people/new`.

## The remote keypad (WEB-018..021)

- **One WebSocket**, `/api/v1/screen` (`panel_web/src/web_screen.c`): the sign-in cookie is checked
  before the upgrade (pre-handshake), the page's first message carries the CSRF token and the
  video it decodes; one viewer, the same session opening it again takes over. Pictures are sent
  only when LVGL drew something (`board_display_stats().refreshes`), at most two ahead of the
  page's acknowledgements (a slow link lowers the rate instead of piling up; the old JPEG poll
  sent 100-200 KB every frame and held the single HTTPS worker while it did, which is what
  delayed the "up" of a press). The page pings every 5 s; the panel lets a silent page go after
  20 s. `max_open_sockets` 4. The old `frame` / `control` routes stay for tools.
- **H.264** (WEB-020): `esp_h264` 1.4.1 (component registry), the P4's hardware encoder: the PPA
  turns the RGB565 copy of the screen into packed YUV 4:2:0 (O_UYY_E_VYY, BT.601 limited), 1024 x
  608 cropped to 600 in the SPS, fixed QP 26, key picture on the page's request; the encoder
  exists only while someone watches and only with 160 KB of internal RAM left (it takes ~82 KB),
  else JPEG over the same socket. The SPS of every key picture is rewritten for no reordering
  (`components/core/h264_sps`, 3 host tests): Chromium's hardware decoders otherwise hold ~7
  pictures back. The browser decodes with WebCodecs (`optimizeForLatency`), falling back to JPEG
  where it can't (Firefox on Android, a non-secure page). `CONFIG_ESP_H264_DECODER_IRAM=n`.
  The page offers **Video (H.264) / Still pictures (JPEG)**, remembered in that browser: the way
  out if the video ever looks wrong on a panel (esp_h264 issue #21, see Tasks), without a new
  firmware.
- **Taps are pressed and released by the panel** (WEB-018, the double key fix): a quick click
  (under 250 ms, under 8 px of movement) is one `tap` message; the panel presses at once and lets
  go 30-250 ms later itself, well under LVGL's 400 ms long-press repeat, however slow the
  network. A hold or a drag is streamed (`down`, `move`, `up`), the page repeating the position
  every 500 ms so the panel keeps a hold (the 2 s Panic) and lets go 2 s after the last one.
- **Being on the page is control** (WEB-019): no Take control. A touch at the panel's own screen
  keeps remote presses out for 3 s after it; a code typed or a protected page opened at the panel
  keeps them out until it's done (`ui_dev_control_begin()`); the page says which.
- **The panel shows it** (UI-039): a steady red banner in the status bar while a session is
  open, the screen saver off; a tap on it at the panel (a remote press on it is ignored) pauses
  the session (no pictures, no presses) and asks *Allow* / *Turn off remote access* (web and
  developer off, no code: it only protects; the installer turns it back on at the panel). The
  pause belongs to the panel: reconnecting doesn't undo it; only *Allow*, or remote access going
  off, ends it (`dev_adapter.c` reports it with every session, `ui_app_remote_session()`).
- **Typing** (WEB-021, UI-040): printable keys and paste go to the text sheet or code pad on the
  panel's screen, Backspace / Enter / Escape as keys; nothing while this page's own form fields
  have focus, nothing for browser shortcuts.
- **Switching tabs**: the canvas and its last picture stay (it used to revoke the image's blob
  URL, which Chrome then drew as a broken image); a background tab stops the pictures and lets
  the panel go after a minute.
- **PC mock**: `components/services/panel_web/web/mock-screen.cjs` streams real H.264 (every macroblock I_PCM, an
  unchanged screen an all-skip P picture) or PNG, so the page's decoder path runs in a browser.
- **Threads** (`web_screen.c`'s header): the HTTPS worker sends everything; the picture task and
  the UI only queue work, under the lock that `web_screen_stop()` takes before the server stops,
  so nothing reaches a stopped server; every queued message names its viewer (number and
  socket), and the server's close callback (`web_screen_closed()`, panel_web's `close_fn`)
  forgets a socket before its number can be reused. Messages are written whole
  (`send_all()`: esp-tls may return a short count and httpd's own WebSocket send takes that for
  the whole frame). `GET /frame` and the stream use one encoder lock and separate JPEG buffers;
  a message's strings are wiped before they are freed.

## Configuration coverage

`components/services/panel_web/src/web_config.c` contains the typed field tables. `panel_web.c` and
`web_extra.c` map actions to existing alarm/platform services and `ui_backend_t` capabilities.
The browser renders the returned schemas instead of maintaining a separate copy of values.
All currently implemented local configuration domains are exposed; future hardware and
planned backup/restore are not presented as implemented options.

The authority column is the first delivery's; the per-path table in
"Browser = panel" (WEB-016) decides: display and sound are open to anyone signed in; network and time to a partition master too; integrations, the SD card and
Software updates to the system master and the installer, with the update server and the channel the
installer's alone (OTA-022); a signed file upload to the installer alone.

| Area | Browser/API coverage | Authority and persistence |
|---|---|---|
| Display and sound | Brightness, saver brightness/delay, Chime volume (`chime_volume`), Voice volume (`voice_volume`, listed with the voice switches; AUD-032) and Beeps volume (`volume`, once Speaker volume; each 0-100), key clicks, the clock (the region's, 12 or 24 h), language and region (docs/features/37, lists from the panel); `config/display`. Announcements (BL-202, docs/features/08 voice-first-build.md D06, D14; the approved proposal `mockup/proposals/BL-202-voice.js`): the three switches (`voice_enabled`, `voice_status`, `voice_chime` in `config/display`, saved as each changes; the walk test has none, it always speaks, docs/features/40 WALK-016), the voice language, whether the voice is ready and the Draft tag, Preview through the Remote keypad, and the signed voice pack's upload (`voices`) | Anyone signed in (WEB-016); the language and region a master or the installer, save preferences then apply through UI adapter; zero or ≥10 s saver delay. A save keeps every stored preference it doesn't name and then hands the switches and the Voice volume to `panel_voice_set_prefs()`; the UI adapter applies the three volumes to the sounder; Announcements turn on only with a ready voice; the upload is the system master's and the installer's, disarmed and quiet |
| Network | SSID/password, DHCP/static IP, mask, gateway, both DNS; `wifi`, `config/network`, Wi-Fi scan, connection check | Master/installer; saved 120 s trial, explicit confirmation, previous profile restored on timeout/reboot |
| Time | Timezone and NTP server; `time` | Master/installer; existing validated NVS setters |
| Home Assistant | Enable, pair, unpair, status; `home_assistant` | Master/installer; existing pairing service, only if included |
| MQTT | Enable, host/port, user/password, TLS policy, discovery/commands, prefix/id, custom CA | Master/installer; typed backend setter, secrets write-only |
| ntfy/Pushover | Enable, servers/topics/tokens, categories, silent events, emergency/sound, CA, topic generation, explicit test/status | Master/installer; existing provider setters, tests never sent automatically |
| Alarm programming | Every `alarm_options_t` field, all partitions/zones/devices, panic-key assignments; `alarm` | The signed-in installer (CORE-PROG-011), expected config revision, atomic enter/apply/exit on alarm task; engine validates complete candidate and idle state |
| Zones and their sensors (BL-134) | Installer > Zones, first in its group: every zone with its type, where its sensor is and how it is now (zones with no sensor named first), then how each sensor source is doing as the last section (Sensor sources is no longer a menu entry; #/zones/setup, #/zones/N, #/binding/N and #/sensors open the new pages). One page per zone, in the touchscreen's groups and words (WORD-006): how the zone is right now; The zone (its Spoken name read-only, with Choose words on the panel opening the Remote keypad, BL-202 D14; every role's Zones > All zones card shows the same line, without the link); Its sensor, picked from what each source sees (wired terminals, Home Assistant with a search, radios while the panel listens; a sensor another zone uses says which and can't be picked, one that just changed goes first - on 433 MHz the source's order, with "Found it" and when each was last heard, and a tamper or low-battery input lists only its own sensor's codes, as the touchscreen does (RF-011, RF-021) - an address can be typed one tap deeper), with its mapping, supervision, response time, hold (433 MHz and Z-Wave only, RF-022) and the tamper and low-battery inputs; What it does (only the options the type has); Advanced; removing the sensor or the zone apart. Choices stand in for typed seconds. Changes wait in one bar ("N changes") and are saved with one review, the zone and its sensor together, with no code (BL-135); leaving the page drops unsaved changes. Removing a zone also frees its sensor. `sources`, `binding/<zone>`, `sources/search`, `sources/learn`, `zone-sources` (installer: `{zones:[{zone, address:{kind, input, id, ref}}]}` for every zone with a sensor, so the list is one request), `zone-status` | Installer for binding; existing source capabilities and zone service; RF433/H2 hardware not invented |
| Users and codes | Add/update/delete, names, authority, partition masks, own/master/installer code operations; `people`, `installer-code` | Existing master/installer authority matrix, as the signed-in person (CORE-USR-028, CORE-PROG-011); no code/hash/salt retrieval |
| Storage | SD details, check/eject, generation-bound format; internal-flash accounting; `storage` | Master/installer and idle gate, literal destructive confirmation for format |
| Firmware (docs/features/13) | Software updates: what is offered, check, install, Stop; a run's stage, level bar and steps, kept in view through the panel's restart and said at the next sign-in; the Wi-Fi module's update; the update server (its domain, checked first, its signing key read and ticked with the installer's password when new) and the channel, held while an update runs and not with the factory installer code (`updates`: `server_check`, `server`, `channel`, `cancel`); developer signed file upload/stage/install/reconnect | System master and installer for the page, Stop included (Stop is allowed while armed); the server and channel are the installer's alone (OTA-020..OTA-022); idle and shared signature/board/profile/version/rollback checks; raw upload additionally needs Developer mode |
| Remote access | Web/Developer switches; `remote` and local Installer controls | Installer, persisted mode; disabled web has no recovery listener |
| Factory reset | Existing reset, optional event-log erase; `factory-reset` | Installer and a recent password, explicit confirmation and existing idle/programming rules |
| Backup and restore (docs/features/14 CFG-005, CFG-006) | The encrypted backup file downloaded (`POST backup`: password, me_password) and restored (`PUT backup`: the file, `X-Backup-Password` and `X-Panel-Password` as hex) | Installer and a recent password; a restore every partition disarmed and quiet, the whole file checked before anything is written, then a restart |
| Status and activity | Panel identity, partitions, versions, memory/display health, public event sentences | Anyone signed in, their own partitions (WEB-016); shared notification wording, duress/silent events filtered |
| Z-Wave (docs/features/36) | Radio, devices, SmartStart list: register by QR code (phone camera where the browser reads QR codes) or 40-digit key, turn off/on, forget; add and remove devices with the PIN typed on the computer, a device's actions, rebuild routes, restart the radio; the Radio page (diagnostics, the radio's own software: check, install, or upload its maker's .gbl - `PUT zwave-radio`); `zwave` | Installer, not while armed (and every partition disarmed); the panel checks every code again; a sensor's PIN never leaves the panel |
| Zigbee (docs/features/20 devices.md §14, BL-145) | Installer > Zigbee, after Z-Wave: the radio (its states, Try again when paused, Restart), the devices (not heard first; power, signal, last heard, how each joined, what uses it), adding for 3 minutes with live progress, an install code typed or scanned and kept waiting, install codes waiting (Forget), the channel (noise per channel, Quietest), the network reset (asked twice, then the installer's password: never a keypad code), Radio details (LQI and RSSI per device); a device's page: its facts and inputs, Blink its light, Check it now, Set it up again, Replace, Remove, Forget, Rename, and a siren's, keypad's or key fob's settings (a siren's supervision has no Off); `zigbee` (`?device=<id>`), `caps.zigbee` | Installer, not while armed (the page then looks only); the panel checks every install code again and never sends one back |

Blank secret fields preserve saved credentials unless their explicit clear control is selected.
Review screens mask secrets. Alarm programming has revision conflict protection; unrelated
platform settings use their existing setters and are not a cross-domain transaction/export.

## Architecture and bounds

- `components/services/panel_web` embeds `panel.html`, `panel.css`, `panel.js`, `panel-pages.js`,
  `panel-keypad.js` and `panel-words.js` (the words and this browser's language, docs/features/37)
  in signed application flash and serves HTTPS 443 (files revalidate by
  ETag = the image's ELF hash, so a reload costs a 304, not the bundle). The files go in gzipped
  (`gzip_asset.py`, 185 KB to 59 KB) and a browser that accepts gzip gets them as they are
  (`Content-Encoding: gzip`, `Vary: Accept-Encoding`); any other client (curl, the HIL scripts)
  gets them inflated by the ROM's `tinfl`, under its own ETag (`"<hash>p"`). The header logo is
  embedded in `panel.html` as a PNG, so it works offline under the data-image CSP. Its internal HTTP control UDP port is 32770,
  distinct from the developer listener's 32769. LWIP socket budget is 24.
- Beside it, only while it runs, a plain HTTP listener on port 80 redirects to it (WEB-023,
  `components/core/http_redirect`): two sockets, a 4 KB task, control port 32771, GET and HEAD
  only.
- Four bounded sessions: random 256-bit identifiers and CSRF tokens, Secure/HttpOnly/
  SameSite cookies, origin/fetch-site checks, 20-minute idle and eight-hour absolute lifetime.
  Sessions expire on reboot, mode changes, user record changes (code, role, name) and browser
  password changes. When all four are taken, a correct sign-in replaces the one used longest
  ago. Sign-in: user name + password
  (`web_login.c`), 2 s between attempts and its own gate (five misses, then a minute doubling
  to 15 minutes).
- JSON input is bounded to 49,152 bytes, depth 12, member/node budgets, receive deadlines,
  strict integer/range/type validation, duplicate-member and embedded-NUL rejection.
  Sensitive parsed strings are cleared. CSP, no-store and framing restrictions apply. `people`
  and `login` give no code oracle; no request carries a keypad code; certificate
  enrolment and downgrades are the installer's; a refusal that leaves a body unread closes the
  connection; TLS receive pauses are retried up to the request's own deadline; the session cookie
  is read by name, and a query string never changes the API path; 64-character WPA2 keys are
  accepted; the configuration copy is wiped on every error path.
- Memory: cJSON, configuration copies, snapshots and request bodies come from PSRAM
  (`cJSON_InitHooks`, `web_alloc()` in `web_mem.h`, no silent internal fallback), and mbedTLS
  allocates from PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`); the upload buffer and the HTTPS
  task stacks stay internal (flash writes). Why: before this a connected browser took internal
  free RAM from ~203 KB to 147 KB (largest block 60 KB), and a shortage of it breaks code hashing
  (B001); with a browser signed in the panel now idles at ~194 KB free, largest block 106 KB.
  Every TLS user shares the PSRAM policy (browser, developer service, MQTT, HA link, ntfy,
  Pushover, OTA downloads); the P4's AES DMA driver handles PSRAM buffers. `status.health`
  carries `internal_largest_kb` and `web_stack_free` (the HTTPS task's stack head-room, for
  trimming its 16 KiB with evidence).
- Product API `/api/v1` and enrolled workstation `/dev/v1` remain separate authorization
  surfaces. No arbitrary shell, memory/NVS access, unsigned firmware or browser backdoor.
- The first delivery's polling path (`frame`, `control`), which tools still use; the page itself
  streams over the WebSocket (The remote keypad):
  P4 capture copies RGB565 under the stable rendering lock into ~1.17 MiB PSRAM, then uses
  hardware JPEG outside that lock with a bounded 768 KiB output buffer. Browser uses a
  board-advertised 100 ms target (up to 10 fps on P4), one outstanding request and measured
  frame-rate reporting (5.7 fps over Wi-Fi with a 161 ms median response, 2.7 fps while flash
  was being erased and written; bandwidth, other viewers and flash activity affect delivery, so
  10 fps is a target, not a claim). Elapsed transfer/decode time counts toward the interval; a slow link
  does not accumulate requests or add a fixed one-second wait. Server capture remains bounded. Busy frames retain the last safe image; private or
  unauthenticated state hides it. No ISR or display refresh timing work was added.
- One remote pointer owner across developer/browser adapters, boot identity and sequence
  checks, bounded leases, local-touch precedence and private-page cleanup. Browser renewal
  never reacquires after local takeover. Touch input precedes the next capture, queued drag
  positions coalesce, and renewals run once per second rather than once per frame. Leaving
  cancels in-flight capture. Board capabilities supply size, capture interval and touch.
  (The browser's open session is its lease, WEB-019; there are no private pages.)
- HTTPS sockets use TCP_NODELAY. Authenticated frame activity temporarily requests Wi-Fi
  without modem power saving; the network task restores the previous mode after five seconds
  without viewing. No persistent Wi-Fi settings change.
- `ALARM_FEATURE_HA`, `ALARM_FEATURE_MQTT` and `ALARM_FEATURE_NOTIFY` condition dependencies,
  initialization and touchscreen callbacks. API manifest/routes follow actual callbacks.
  HA source/home-control require HA. Notification providers currently share one compile flag.
  Included but runtime-disabled integrations remain configurable. Generic JS render code may
  remain in the asset bundle; an excluded integration has no service, route or card.
- Network trial backup is stored before applying changes; timeout/reboot restore it. A lost
  HTTP response during Wi-Fi reconfiguration is an uncertain connection, not success.
- Browser upload streams to the inactive OTA slot, verifies signed identity/digest and stages
  a random operation. Only its expected ELF reconnect can finish startup probation. Failure
  rolls back. Mode settings and workstation trust survive successful normal/release updates.

## Sign-in (2026-09-23)

**Design:** a sign-in is enabled per user on the panel, with a password entered there; the user
name is the name on the Users screen. The installer's password is its own, not the installer code
(why below).

| Who | User name | Password | Set where |
|---|---|---|---|
| A system master | their name on Settings > Users (not case-sensitive, spaces around it ignored) | 12-64 characters of their own | Settings > Users > the user > **Allow browser sign-in** (a master code opens the page) |
| The installer | their name on Settings > Users, like everyone (**Installer** from the factory; a blank name still answers to it) | 12-64 characters of their own - **not the installer code** | Settings > Installer (installer mode) > **Allow browser sign-in** |

**One rule for everyone:** the installer signs in with their name on the Users screen, not the panel
ID. It is one rule on the panel (`web_login.c` `login_name()`) and on the touchscreen
(`ui_login_name()`); a password can't be set for a name another user already has, the installer's
included (a later rename can still make two alike; each is then tried). The rule is checked by
review and the preview, not by a host test (`web_login.c` needs ESP-IDF). The panel ID, which
nobody could guess, is no part of the sign-in: the installer's password and the sign-in gate
protect it, as they do everyone's.

Partition masters and standard users can have one too (WEB-017), named the same way, and whoever
manages a user can also set it in the Panel Manager's People page (temporary until changed at
the first sign-in, unless they choose that the person keeps it); see "Users in the browser".

A password under 12 characters is refused as a keypad code, with where to set a browser password,
and is not counted as a wrong sign-in (B010): the installer code typed at the sign-in before a
browser password exists would otherwise lock the person out. Codes still do not sign in.

Research behind it (NIST SP 800-63B-4 and the OWASP ASVS / Authentication Cheat Sheet):
- **A keypad code is a poor network password.** 4-10 digits (10^4-10^10), typed at the door
  where it can be watched, and the installer code also opens installer mode at the keypad.
  With the 2 s spacing and a 5-then-doubling gate, a 4-digit code falls to an online guesser in
  about two weeks, and one read over a shoulder would open the panel from the network as well.
  So the installer's browser password is separate from the installer code, as it is for the
  masters; the installer's user name is their own name (above). The code is still asked where it was
  (alarm programming, sensor links, people and codes, factory reset), which makes those
  changes two-factor.
- **Length, not composition** (NIST: no composition rules, allow long passphrases and all
  printable characters, refuse obvious choices). Minimum 12 (NIST asks 15 for a password that
  is the only factor; here it is joined by the online limits below and, for anything that
  matters, the keypad code - raise `WEB_PASSWORD_MIN` if that changes), maximum 64. Refused:
  one character repeated, a password containing the first word of the user name.
- **Set on the panel only.** Setting or changing a password needs someone at the screen with a
  master code (or the installer in installer mode), so a stolen browser session cannot lock
  the owner out or keep itself alive with a new password. (Relaxed: a person may change
  their own in the browser, below, and whoever manages a user may set one in the Panel
  Manager as themselves.)
- **Stored as PBKDF2-HMAC-SHA256**, 16-byte random salt, 20,000 iterations (the count is kept
  per password so it can be raised; about 1.0 s on the P4 with hardware SHA, through
  `code_hash_derive()`), in NVS `weblogin/uNN`. The flash is not encrypted, so the slow hash is
  what protects a password copied out of a stolen board.
- **Bound to the person:** each password carries a digest of the user's slot and code record.
  Deleting the user, or anyone else taking over the slot (a new user, a factory default), ends
  it; a code change by the same person carries it over (the `user_changed` event rebinds it);
  becoming a user who can't have one (arm-only, guest or duress: `web_login_eligible()`)
  removes it. The listener only sets flags (it
  runs on the alarm task, which must not wait); the next reader reconciles with a fresh copy
  of the configuration.
- **No oracle:** a wrong user name and a wrong password answer the same `401 invalid_login`
  and take the same time (an unknown name still runs the full hash). 2 s between attempts
  (429), and a sign-in gate of its own: five misses, then a minute's wait doubling to 15
  minutes, forgotten after an hour without one. A wrong password typed for a guarded change
  ("Recent password") counts here too: it is the same guess.
- **Sessions follow the credential:** the session proof hashes the user record and the
  password record, so a new password, a sign-in turned off, a code change, a new role or a
  rename signs that user's browsers out.

Who can have one (WEB-017): the installer, system and partition masters and standard users, each
seeing only what they see at the panel ("Browser = panel"). Two users with the same name cannot
both enable one; a rename that makes two alike tries both.

**Code:** `components/services/panel_web/src/web_login.c` and `include/web_login.h` (store,
reconcile, check), `panel_web.c`
(`login`, session proof, `status.name`, `people[].web_login`/`web_login_possible`, oldest-session
replacement), `web_extra.c` (`web_gate_t`), `platform_esp/src/code_hash.c` (`code_hash_derive`),
`app_main.c` (init + listener), `alarm_ui`: `ui_backend_t.web_login_*`, `ui_about_t.panel_id`,
`ui_login_rows()` in `ui_settings.c` (Users) and `ui_installer.c`, About's "Panel ID" row
(docs/features/07 UI-038). Browser: `panel.js` sign-in form, `panel-pages.js` People column.

### Temporary password and changing it in the browser (2026-09-23 morning)

A person away from home can sign in and choose a password with nobody at the panel's screen:
- **Service console:** `web password <user>` makes the panel generate a temporary password
  (16 random characters from an alphabet without 0/o/1/l/i, in four groups, about 79 bits) and
  prints it once; `web password <user> off` turns the sign-in off. Physical service console
  only (USB), so not in release firmware. Nothing default or guessable is ever set.
- **Temporary means change first:** the record carries `REC_TEMPORARY`; a session opened with
  it answers `must_change` at sign-in and `403 password_change_required` to everything but
  `status`, `logout` and `password`; the browser shows only "Choose your password".
- **Change your password in the browser** (`POST password {current, password}`, the header's
  "Change password"): the current password first (a wrong one counts toward the sign-in gate),
  the new one under the panel's rules, not the same as the current one. The session goes on
  with the new password; the user's other sessions are signed out. This relaxes "set on the
  panel only" for changes by the signed-in user themself; first setting still needs the panel
  (or the console).

## Test and acceptance

- **In a browser on the PC:** `components/services/panel_web/test/panel_pages.test.cjs` renders
  every page for each role against `web/mock-panel.cjs` (which mirrors the access table) and
  checks the refusals, the lock while armed, partitions and the update server and channel, who manages
  whom, the date and time, the sign-in, the temporary password, the Z-Wave pages and the
  content-blocker names; `panel_frames.test.cjs` drives the remote keypad page against
  `mock-screen.cjs` (taps, holds, drags, typing, video or still pictures, a background tab,
  reconnecting). CI runs both (`.github/workflows/host-tests.yml`), and `manager_mockup.test.cjs`
  (the mockup, WEB-026).
- **The mockup** ([mockup/README.md](mockup/README.md)): `node
  docs/features/31-Browser-Panel-Manager/mockup/build.cjs` builds `build/mockups/manager/index.html`,
  every page with its pretend panel in the page, for looking at a page in any role and state, and
  for proposals.
- **Host tests:** `components/core/h264_sps/test/test_h264_sps.c` (the low-delay SPS, WEB-020),
  `components/core/http_redirect/test/test_http_redirect.c` (the port-80 redirect, WEB-023),
  `components/core/alarm_core/test/test_users.c` (CORE-USR-012/025, WEB-017).
- **UI preview** (docs/features/07 screens): the sign-in rows `116`-`118`, the installer's users
  `119`, the red banner and typing `120`, the pause `121`.
- **On a panel**, run by hand against an explicitly selected panel with its certificate pinned
  (never in CI): `tools/tests/panel_web_hil.py` (reads and refusals),
  `panel_web_configuration_hil.py` (reversible configuration), `panel_web_modes_hil.py` (web
  and developer off, with USB recovery), `panel_web_ota_hil.py` (signed upload),
  `panel_web_fps_hil.py` (capture rate) and `panel_web_browser_test.py` (a loopback transport
  for browser automation; it reads, signs in and sends pointer events only, over an exact pinned
  TLS backend, and never bypasses a browser certificate warning).
- **Verified on a panel** (v1 accepted on a P4 on 2026-09-22, panel disarmed throughout):
  - The HIL scripts: certificate pin, assets, authentication, CSRF and origin, role denial,
    secret redaction, malformed, duplicate, deep, NUL and out-of-range input, 20 JPEG frames,
    logout; reversible configuration, with confirmed and timed-out network trials (a trial
    restored its previous profile across a reboot); Developer off closes the developer listener
    with 443 alive, web off closes both, and local recovery restores them with old sessions
    refused; signed browser uploads with staging, exact-ELF reconnect and probation (earlier
    startup failures rolled back by themselves).
  - A minimal feature-off image (HA, MQTT and notification symbols absent) booted with an empty
    manifest and no cards, and refused the disabled APIs.
  - Under `display stress 20` (about 200 sectors erased and written) with a 60 s browser load
    (230 API pages, 272 full frames, no errors): 55.9 fps, 0 late frames, 0 guard trips, 0 I2C
    errors, no crypto faults.
  - The gzipped assets: all five files byte-exact both ways (chunked when inflated), a 304 per
    representation, `gzip;q=0` plain, 404 and the port-80 redirect. `panel_web_hil.py` records
    the refusal of `alarm` to a grant session when the grant's user isn't the installer.
  - Sign-in: a code-only attempt and an unknown user both answer `401 invalid_login` in about
    1.5 s, the 2 s spacing answers 429; the temporary-password flow (`must_change`, 403 on other
    pages, the change answers 200 and the same session then opens People, the new password signs
    in without `must_change`). A signed update reached the second panel over the VPN on
    2026-09-24.
  - Not done on hardware: setting a password at the screen and signing in with it, the remote
    keypad rework, the Overview's arming, the Panel Manager's code guessing (B005, BL-025).
  - The Activity page's "Failed to fetch" was a content-blocker rule on the API path name
    ([B007](../../bugs/B007-activity-blocked-by-content-blockers.md)); the page tests check that
    no path matches a known rule.

## Remaining commissioning and future work

What is left is tracked as boxes in [Tasks](#tasks).

1. Verify first browser certificate trust from local Installer, then exercise the real
   site-to-site VPN and a second panel with its own identity. Establish that remote route
   before a panel is installed beyond physical reach; this feature does not create a VPN.
2. Test the second site's actual receiver/engineering hardware when available. RF433 development,
   hardware boot recovery and a remote USB bench host remain separate Feature 25/30 work.
3. Long-duration soak and actual power-loss/NVS-fault cases remain beyond these bounded checks.
   Destructive settings/code changes were not exercised on a configured device.
4. Named helper accounts, delegated family permissions, passkeys, household CA/certificate
   renewal UI, central fleet dashboard, cross-domain backup/restore and physical second-board
   acceptance are future work. Browser access is by name and password for the installer,
   system and partition masters and standard users (WEB-017); it does not claim the
   broader proposed identity system is complete. Worth adding next: sign-ins and failed sign-ins
   in the event log (a new `alarm_event_type_t`, appended), and passkeys (WebAuthn) once the
   certificate is trusted.
5. Every word of the pages goes through the catalog ([Globalization](../37-Globalization/README.md),
   docs/features/37 G3, done in software): `t()`, `ht()` and the rest in
   `panel-words.js`; the panel writes `/i18n/<locale>.json` from its own tables (no copy in the
   image), the page opens in the panel's language (`/i18n/panel`, public like the sign-in page's
   words) unless this browser chose another, and requests carry `X-Panel-Language` so what the
   panel words for the page comes in it; translations always inserted as text (I18N-008,
   I18N-031); the mock serves the test languages, and a real one after `tools/i18n.py web`
   (`MOCK_I18N=build/i18n/web`). Spanish is a draft (G4): a development build lists
   it as "Español · un borrador, para pruebas", a release build doesn't carry it; the language,
   region and clock choices `config/display` sends are named in the page's language.

Next concrete step after this local acceptance: commission a second panel over the existing
VPN and verify its own URL/certificate, browser login/control, signed OTA and rollback.
[DESIGN.md](DESIGN.md) preserves research sources and the broader proposed roadmap; it is
historical/future design, not a statement that all its acceptance gates are already satisfied.

## Tasks

Each open box names its row in [the backlog](../../BACKLOG.md). v1 was accepted on a panel on
2026-09-22 (Test and acceptance); the checks on panels below can only be ticked from a panel.

**On panels**
- [ ] First browser trust of a panel's certificate, from the fingerprint on its local
  Settings > Installer, without bypassing the warning (first-use commissioning)
  (BL-080)
- [ ] The remote keypad rework on a panel: open it over the VPN; check the H.264 picture
  (esp_h264 issue #21 reports corrupted P pictures with DMA-written input on rev 3.1 silicon;
  check on the panel's own revision); type a password on the keyboard sheet with the computer's
  keyboard and by clicking; hold Panic; tap the banner at the panel; watch `display` for late
  frames while streaming, and internal RAM with the encoder running (BL-080)
- [ ] A browser password set on the panel's screen (Allow browser sign-in, with a master code or
  the installer code at the screen), then a sign-in with it (BL-080)
- [ ] A second panel over the site-to-site VPN with its own identity: its URL and certificate,
  browser sign-in and the remote keypad, a signed upload and its rollback (a signed update
  already reached it over the VPN on 2026-09-24, "Test and acceptance") (BL-035)
- [ ] A long soak with the Panel Manager in use, real power-loss and NVS-fault cases during its
  writes, and the destructive settings and code changes, on a panel that is not in use (BL-080)

**The Overview as the alarm command centre (BL-113, built 2026-09-29)**

The Overview shows what is going on and arms and disarms, without a keypad code once signed in
and without an Open remote keypad button. The page keeps its name, Overview (most people name
their alarm Home), and every card names its partition. The design, the mockup reviews and the
research are in docs/features/38.
- [x] Mockup agreed: a card per partition (state, Away / Stay / Night, each level's own options),
  confirmation before a normal disarm only, the zones that tripped in order, tiles (BL-113, 61f6ffe)
- [x] Arm and disarm as the signed-in person without a code: `alarm_engine_command_as()`
  (CORE-USR-027, as a button zone's rule, CORE-USR-015), `POST partition` (WEB-024) (BL-113,
  61f6ffe)
- [x] The status the Overview needs: delay left, who armed, readiness, zones in order, chime,
  `alarm_kind` (WEB-024) (BL-113, 61f6ffe)
- [x] The Overview, Zones and Troubles pages built, words through the catalog, tried in the mock
  (BL-113, 61f6ffe)
- [ ] On a panel: arm, disarm and cancel arming from the Panel Manager (the event and a phone
  notification reading "Panel Manager"), the countdown, an alarm's "What happened" with real
  sensors, "Alarm occurred" and Clear alarm, Zones and Troubles against real zones; a grant
  session refused (BL-113)

**Usability redesign (BL-134)**

An audit on 2026-10-02 found the pages that make you jump around, read raw values or scroll far to
save; each fix is drawn as a proposal in [the mockup](mockup/README.md) (WEB-026) with the
Overview's and the walk test's patterns: what is true now on top, steps in place of dialogs, one
solid button a card, the touchscreen's words.
- [x] Installer > Zones and a zone on one page: its own menu entry, its sensor picked from each
  source's live candidates, one review; `GET zone-sources`; the sources as its last section
  (BL-134, 7d0ad0b)
- [ ] Proposals for Partitions and devices, System options, the Z-Wave rows, Access's restore row
  and Network's connection check (BL-134)
- [x] No keypad codes in the Panel Manager, a recent password before the changes that let someone
  in or wipe the panel, programming as the signed-in installer (BL-135, 7d0ad0b)
- [x] Software updates: the update server, its key and the channel for the installer (BL-186, OTA-017..OTA-022)
- [x] Software updates follow a run from download to self-test, with Stop, across the restart (BL-188, bc7b2ca)
- [x] The installer's Overview shows state but no arm or disarm buttons (BL-187, 96c72ae)
- [x] People and codes, a page per person saved from a bar; `temporary`, `code_min`; the eye
  (BL-136, 7d0ad0b)
- [x] Troubles as the touchscreen lists them, with what to do; `GET troubles`, `trouble_count` and
  `trouble_sig` (BL-137, 7d0ad0b)
- [ ] On a panel: the zone page, no codes, the password asked again, a person's page and Troubles
  against real zones and troubles (BL-138)

**Later**
- [ ] Arm with the zones to bypass named, bypassed and armed in one alarm-task call (undone if the arming fails), so a zone that opens in the moment between the page's last look and the command is never bypassed with the others (BL-115)
- [ ] Sign-ins and failed sign-ins in the event log: a new `alarm_event_type_t`, appended
  (BL-064)
- [ ] Passkeys (WebAuthn) once the panel's certificate is trusted (BL-064)
- [ ] Named helper accounts, delegated family permissions, a household CA and certificate
  renewal in the Panel Manager, a central fleet dashboard (BL-076)
- [ ] The Panel Manager on another physical board (WEB-014: board capabilities from a second-size
  adapter, then a real board; the "physical second-board acceptance" of item 4 above) (BL-091)
- [ ] The Panel Manager installs no language or voice packs (ADR-053): `PUT languages` and `PUT
  voices` go with their upload controls (a drawn proposal first, WEB-026), which also ends `PUT
  languages` holding the HTTPS worker during a slow upload (a code-review finding); the language list
  stays, and choosing a language downloads its pack and its voice (BL-205) (BL-206)

**Tracked elsewhere**
- Duplicate of docs/features/14 ("Backup/restore JSON codec", CFG-005/006; BL-002): backup and
  restore across every domain, the Panel Manager included.
- Duplicate of docs/features/30 ("Verification and remaining acceptance": an independent bench
  host and receiver-specific RF capture, future work there with no backlog row; "Update safety
  and recovery": the on-site USB bench host as the independent recovery option) and
  docs/features/25 (the receiver on a panel, BL-022): the second site's receiver and engineering
  hardware, hardware boot recovery and a remote USB bench host, item 2 above.
- Owed on a panel and tracked in the bug reports: the Panel Manager's code guessing
  ([B005](../../bugs/B005-panel-manager-code-guessing.md)) and a keypad code at the sign-in
  ([B010](../../bugs/B010-keypad-code-at-browser-sign-in.md)), BL-025; a remote press cut off
  mid-hold that finished the Panic hold by itself
  ([B004](../../bugs/B004-remote-press-completed-panic-hold.md)), BL-024.
