# 14 · Configuration and installer programming

**Status:** Implemented v1 - versioned storage, installer mode, factory reset, power-up recovery
and the access table are host-, preview- and page-tested; the Panel Manager follows the table,
the installer manages every user and sets everything at the panel, and the backup file
(CFG-005, CFG-006) is built and host- and page-tested. Left: the installer's new pages and the
role rules tried on a panel (BL-024, BL-080), the backup file on a panel (BL-002). · **Milestone:** M4
(backup/restore: M5) · **Depends on:** 01, 02, 03, 07, 15

## Goal

Browser configuration is implemented by [Feature 31](../31-Browser-Panel-Manager/README.md).
Its typed field tables and action handlers cover the implemented local configuration domains,
preserve this feature's master/installer authority and use the existing alarm/platform services.
Alarm programming adds revision checks and atomic enter/apply/exit on the alarm task. Network
changes add saved rollback on timeout/reboot. Normal firmware includes persistent Installer
Panel Manager (the web server) / Developer mode toggles. See Feature 31 for coverage and measured acceptance.

All panel settings live in versioned, validated structures stored in NVS. They're edited
through an installer mode on the touchscreen (the friendly equivalent of numbered data-field
programming) and can be backed up and restored.

## Reference behaviour
A classic panel's programming is entered with the installer code and a short key sequence
while disarmed, and left with another (one of them also locks out the power-up entry). Settings
are numbered data fields (zone programming, the installer code). A lost installer code is
recovered by powering the panel down and pressing two keys together within 50 seconds of
power-up. Touchscreen panels use menus behind the installer code.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| CFG-001 | Core configuration is `alarm_config_t` (validated by `alarm_config_validate`); platform settings are separate NVS keys | `CORE-CFG-*` |
| CFG-002 | Installer mode needs the installer code, only while every partition is disarmed and silent; entering and leaving are logged and reported; ends after 15 min without activity; nothing can be armed meanwhile; fire, CO, 24-hour zones and panics stay live | `CORE-PROG-001..004` |
| CFG-003 | Changes are applied with `alarm_engine_set_config` (atomic; rejected unless idle) when a page is left, and stored before the call returns | `CORE-CFG-005`, UI |
| CFG-004 | The configuration is stored as a versioned, self-describing blob: newer firmware reads older blobs (missing fields keep defaults), older firmware reads newer ones (unknown fields skipped), frozen fixtures from every format stay decodable | `CORE-CFG-012..017` |
| CFG-005 | Backup: one file the installer downloads in the Panel Manager, always encrypted with a password the installer chooses (PBKDF2 and AES-GCM), holding everything needed to rebuild the panel - the configuration, users (code hashes and salts), zones and their bindings, the Z-Wave keys, node cache and SmartStart list, the integrations' settings and secrets and the Wi-Fi network - but never the panel's own identity (its certificates and keys, the developer enrollment, the panel ID). | `components/core/backup/test/test_backup.c` (CFG-005), `panel_pages.test.cjs` "backup"; on a panel (BL-002) |
| CFG-006 | Restore, by the installer with every partition disarmed, onto the same panel or a replacement board: the file is decrypted and validated in full first, then applied all or nothing, then the panel restarts; a replacement board keeps its own identity and takes over the Z-Wave radio the file's keys belong to | `test_backup.c` (CFG-006), `panel_pages.test.cjs` "backup"; on a panel and a replacement board (BL-002) |
| CFG-007 | Factory reset from installer mode on the touchscreen (hold to confirm) or the service console: wipes NVS (and the event log if asked), logs `factory_reset` first, restarts with the factory codes (installer 69420, master 123456) and the setup wizard. **No physical button is ever needed** (the boards sit behind a case) | preview, HIL |
| CFG-008 | Validation warnings, not errors: entry time + report delay > 60 s, no night zones while Night arming exists | preview |
| CFG-009 | **Lost installer code**: for 2 minutes after power-up, with every partition disarmed, installer mode opens from the touchscreen without a code; logged and reported (`program_enter`, arg 1); the installer then sets a new installer code without the old one. An installer option turns this off | `CORE-PROG-005..007`, preview |
| CFG-010 | An unreadable stored configuration (damaged, or written by a newer firmware before a rollback) is kept, not overwritten: the panel runs on factory defaults in RAM and raises the `storage` trouble. A stored configuration that decodes but the engine rejects is treated the same way. Only a positively **absent** NVS key ever writes the factory configuration | code review |
| CFG-012 | A load that failed for want of resources (no memory, NVS unusable) is not a verdict on what is stored: the panel retries, then refuses to start rather than replace the installation, and never hands unvalidated configuration to the engine. **An allocation failure can never cause a factory reset** | code review; allocation fault injection still to do |
| CFG-013 | A configuration or runtime write that failed is never reported as durable: it stays outstanding, is retried with backoff, coalesces to the newest state and raises the `storage` trouble until it lands. The in-memory state is never rolled back because flash refused (a disarm stays a disarm); `alarm_service_store_status()` reports it ([06](../06-CrowPanel-P4-Platform/README.md) P4-013) | code review; flash fault injection still to do |
| CFG-011 | Access follows the table in [Who can change what](#who-can-change-what): the homeowner (master code) changes Wi-Fi, date and time, integrations and household users without the installer; the installer changes what detects, delays, sounds or reports alarms, in installer mode; both reach the shared settings with their own code, outside installer mode | preview, code review |

## Who can change what

A homeowner who replaces the router must be able to join the new Wi-Fi without calling the
installer, and an installer setting up the panel must reach Wi-Fi too. So settings fall into
three groups: open to anyone, shared by the master and the installer, and installer-only.

The household looks after what changes with the home (network, people,
clock, connected apps) and can test and restart; the installer keeps everything that changes
how an alarm is detected, delayed, sounded or reported, plus the master code itself.

### This panel
| Area | Anyone | Master | Installer | Where |
|---|---|---|---|---|
| Screen and sound: brightness, screen saver, screen off, night hours, volume, key clicks | yes | yes | yes | Settings > Screen and sound |
| Chime on or off (only while disarmed) | every user's code, or nobody's when the installer turned the code off | yes | yes | status bar > Chime, Settings > Screen and sound |
| The clock: automatic (the region's), 12-hour or 24-hour (this keypad) | yes | yes | yes | Settings > Time and language > Clock; Panel Manager > Display and sound |
| Language and region (docs/features/37) | | yes | yes | Settings > Time and language; Panel Manager > Display and sound |
| Language packs: see which are installed | yes | yes | yes | Panel Manager > Display and sound; the Language list |
| Language packs: add one from a file, remove one, fetch one from the update source (docs/features/37 language-packs.md; not while armed or in alarm) | | yes | yes | Panel Manager > Display and sound; the Flasher at install and update |
| Activity (recent events; silent ones hidden) | yes | yes | yes | Activity |
| Your own code (the factory-code banner) | every user | yes | yes | banner |
| Users: add, name, authority, partitions, code, delete | | yes (system master: everyone but the installer; partition master: its partitions' standard, arm-only, guest and duress users) | yes, everyone | Settings > Users; Panel Manager > People and codes (as the signed-in person, no code: CORE-USR-028) |
| Wi-Fi | | yes | yes | Settings > Wi-Fi and network |
| Time zone, time server | | yes | yes | Settings > Time and language |
| Integrations: the Home Assistant pairing, MQTT (broker, discovery, commands on/off), and the notification services ntfy and Pushover - two groups, one entry each | | yes (system master) | yes | Settings > Integrations |
| Panic alarms over MQTT (police, fire, medical, silent): allowed or not, off by default | | sees it | yes | Panel Manager > Integrations > MQTT (the installer's sign-in); the service console's `mqtt panic` |
| Event log with silent events | | yes | yes | Activity > Show private events (LOG-008, BL-131) |
| Automations: add, change, turn off, delete; one that arms or disarms the panel acts as the person who saved it (docs/features/41 AUT-010, AUT-011; decided 2026-10-05: the master decides). Not while armed or in alarm | | yes (system master) | yes | Settings > Automations; Panel Manager > Automations |
| Installer and master codes without the old one | | | installer mode | Installer > Installer and master codes |
| Zones and sensors, delays, siren time, lockout, power options, system name | | | installer mode | Installer |
| Z-Wave: add, remove, replace a device, region, routes, reset the Z-Wave network (every partition disarmed); the device list and a device's page are in the installer menu too (docs/features/36) | | | installer mode | Installer > Z-Wave |
| Arming without a code (keypad quick arm, remote arming), chime without a code | | | installer mode | Installer > Delays and arming |
| Allow less secure automations: automations that disarm, unlock or open (on by default; docs/features/41 AUT-015) | | | installer mode | Installer, beside arming without a code |
| Run the setup again, factory reset | | | installer mode | Installer |
| About (versions, memory, network, System details), whether an update exists or runs | yes | yes | yes | Settings > About |
| Storage information: SD capacity/state, firmware space, flash allocation | yes | yes | yes | Settings > About > Storage |
| SD card: check, safely remove, format (explicit erase confirmation) | | yes (system master) | yes | About > Storage > SD card |
| Software update: see what is offered, check, install, stop (restarts only once everything is disarmed; OTA-022, OTA-027) | | yes (system master) | yes | Settings > About > Software update; Panel Manager > Software updates |
| The update server (a domain; a new one's signing key is read and held to accept) and the update channel (OTA-020..OTA-022) | | | installer mode; in the browser the installer signed in | Settings > About > Installer > Updates; Panel Manager > Software updates |
| Panel Manager browser sign-in (password), for the installer, system and partition masters and standard users: on, off, a new password | | whoever manages the user (never the installer's) | everyone's | Settings > Users > the user; Panel Manager > People and codes (no code). One set for somebody else is temporary at the panel; in the Panel Manager the person setting it chooses whether they keep it or choose their own at their first sign-in. Your own: Panel Manager > Your password, which asks for the current one, never People and codes |
| Panel Manager browser sign-in: the installer's own (user name: their name on Users, "Installer" from the factory) | | | yes | Installer, or Users with the installer code; Panel Manager > Your password (the current one first) |
| Arm, disarm, clear the alarm memory, acknowledge (trouble beeps, and it silences a fire, CO or auxiliary siren remotely, as OFF alone does at a keypad), bypass and the chime from the Panel Manager, as the signed-in person without a code (their sign-in proved who they are); each only in the state it is for (disarm while armed, in exit or entry time or in an alarm; acknowledge always; the rest while disarmed and outside installer mode); disarm and acknowledge also while armed or in alarm, since disarming ends the lock (docs/features/31 WEB-024) | anyone with a browser sign-in, as far as their own code allows it (authority and partitions, the same rules as at a keypad) | yes | not arm or disarm: the installer's code never does, from any source (USR-001, [03](../03-Users-Codes-and-Authority/README.md)) | Panel Manager > Overview; the event names the person and "Panel Manager" |
| Walk test: start one (disarmed, silent, outside installer mode), extend it, see its list and summary in the Panel Manager (docs/features/40 WALK-001; at the touchscreen the list and the summary need no code, WALK-025) | | yes (system master; a partition master on its partitions) | yes | Settings > Walk test; Installer > Walk test; Panel Manager > System > Walk test (a password sign-in); a keypad's code + TEST |
| Walk test: end one that is running (WALK-025) | every code that may disarm the partition; at the touchscreen no code (a question) | yes | yes | code + OFF, a keyfob's Disarm, End on the walk test screen or Home; Panel Manager > Overview |
| Remote keypad (the panel's own screen from the Panel Manager: anything a person at the panel could do, codes still asked) | anyone with a browser sign-in | yes | yes | Panel Manager > Remote keypad; the panel shows a red banner and the person there can pause it or turn remote access off (UI-039) |
| Remote access switches (web server, developer mode), workstation enrolment | | | installer mode / in the browser the installer signed in, with their password again unless typed in the last 10 minutes | Installer; Panel Manager > Access and maintenance, Developer |

"Master" means the system master and partition masters unless the table says otherwise. A code
typed in Settings opens every section it allows until Settings closes (after 5 idle minutes at
the latest); entering installer mode with the installer code counts as that code.

**Planned annunciation controls (Feature 08; not implemented, BL-044):** ordinary voice enable,
voice-chime mode, library voice and preview follow Screen and sound access and lockdown;
chime's code gate still applies. Library/model packages and custom audio uploads/removal are
shared system-master/installer maintenance; custom uploads and TTS assets/output require SD.
Selecting words, a recording or TTS text for a
zone remains installer programming, as do supported system-prompt overrides. Managing an
audio asset does not grant zone-programming authority; in-use assets cannot be removed until
their bindings are explicitly changed/disabled by an authorized installer. All changes require
disarmed/silent state, and Panel Manager enforces the same policy. See
[Feature 08](../08-Audio-and-Annunciation/README.md) for the proposed workflows.

**While armed** (from the exit time on) **or in alarm**, nothing in this table can be changed:
Tools and Settings aren't offered while a partition is armed, user codes can't be added or
changed then, and chime and volume change only while disarmed. Settings stays shut on the touchscreen, arming from
elsewhere closes it, and the engine refuses user and chime changes
([07](../07-UI-LVGL-Pro/README.md) UI-022, [01](../01-Core-Alarm-Engine/README.md) CORE-USR-023).

Deliberate choices:
- **The installer manages every user.** It used to handle only
  its own code and the master's, to keep the family's codes from the installer; some panels
  let the installer into user management, and on this project the
  installer is usually the owner. The system master manages everyone but the installer.
- **The Panel Manager is this table**: each API path has one entry (anyone, users,
  shared, system, installer), checked before its handler runs; nothing from the browser changes
  while the panel is armed or in alarm; a standard user signed in sees what anyone sees at the
  panel (status, activity, the zone list in the panel's words, display and sound, About,
  storage, update status) plus the remote keypad, which is standing at the panel. Enrolling a
  developer certificate and installing older firmware need the installer (a master's enrolled
  certificate would have outlived the master). No browser request carries a keypad code: the
  installer's programming, people and codes and the installer and master codes go as the
  signed-in person, whose role the engine reads again at every change (docs/features/01
  CORE-USR-028, CORE-PROG-011). The person's password again, unless typed in the last 10
  minutes (signing in counts), guards what lets a computer in or can't be undone: the remote
  access switches, enrolling a workstation, downloading or restoring a backup and factory
  reset; a wrong one counts with the wrong sign-ins
  ([B005](../../bugs/B005-panel-manager-code-guessing.md)).
- **Browser sign-in is its own password, not a keypad code** (docs/features/31 "Sign-in"): a
  user name and a browser password set on the panel's screen; the installer's is the installer's
  own, set in installer mode. Whoever manages a user sets or removes theirs, at the panel or in
  the Panel Manager as themselves; one set for somebody else is temporary (in the Panel Manager,
  unless the person setting it chooses that they keep it); it goes with the user or their role.
- **Shared settings don't need installer mode.** Installer mode stops arming; changing the Wi-Fi
  or the time zone shouldn't.
- **The event log needs a master or installer code** (LOG-008), not any user code,
  because it shows duress and silent alarms.
- **Arming without a code, from the keypad or remotely, stays an installer option** (like Quick
  Arm on classic panels), even though the Home Assistant connection itself is the master's.
- Later, the installer may hand single areas to the master (zone names
  and chimes are the obvious candidates; BL-090).
- **Software updates are the household's too.** A master or installer code installs one, never while the
  system is armed or sounding, and a failed update rolls itself back ([13](../13-OTA-Updates/README.md)).

## Design (as built)

### Storage
| Namespace / key | Content | Written when |
|---|---|---|
| `alarm/config` | configuration blob (`alarm/alarm_codec.h`) | installer changes, user changes (`on_config_changed`) |
| `alarm/rt` | `alarm_runtime_state_t` (fixed layout, `ALARM_RUNTIME_VERSION`) | arming, bypass, memory, chime changes |
| `net/*` | Wi-Fi SSID and password, DHCP or static address, POSIX time zone, time server | Settings > Wi-Fi and network, Time and language, setup; the Panel Manager's network page |
| `ui/*` | brightness, screen saver, the screen going dark (`off_ms`) and the night hours (`night`, `night_from`, `night_to`, `night_bright`, `night_off_ms`; [07](../07-UI-LVGL-Pro/README.md) UI-043), volume, key clicks, the clock choice (`clockmode`; `clock24` still written as the clock it comes to, for a rollback), the language (`lang`) and region (`region`) ([37](../37-Globalization/README.md)), the appearance (`appearance`: 0 Dark, 1 Light, 2 Automatic; `contrast`, high contrast; [07](../07-UI-LVGL-Pro/README.md) UI-010) | Settings > Display and sound; the clock on Settings > Date and time; the Panel Manager's Display and sound |
| `sys/setup` | first-boot setup finished | end of the setup |
| `zones/bind` | the binding blob: which sensor feeds each zone ([22](../22-Zone-Sources/README.md)) | a zone's sensor picked or removed (installer mode, or the console while disarmed); first start with zone sources |
| `log/*` | boot counter and the lost-record counters (`lost`, `lostrep`) of the event log ([05](../05-Event-Log/README.md) LOG-010) | every start; when a record could not be kept |
| `mqtt/*` | broker, account, TLS and its certificate authority, discovery, commands, topic prefix ([10](../10-MQTT-and-Home-Assistant/README.md)) | Settings > Integrations > MQTT, the Panel Manager |
| `halink`, `notify`, `ntfy`, `pushover`, `weblogin`, `panelweb`, `devrem`, `devota`, `ota`, `nettrial`, `clock`, `rf`, `zwave`, `zbpanel`, `automations` (and `touch` on the 4") | each service's own settings and state, described in its feature document (09, 13, 19, 20, 23, 25-28, 30, 31, 35, 36, 41) | their own pages |
| `link/*` | a later milestone (the panel link, [11](../11-Panel-Link-Master-Keypad/README.md)) | |

Both `alarm/*` keys are written by the alarm task right after the engine call that changed
them, and the revision the engine reached is tracked apart from the revision NVS holds
(CFG-013): a write that fails stays outstanding and is retried after 1 s, then 2, 4... up to
60 s, always with the newest state, while the panel shows "Storage problem". Nothing about the
engine is undone because a write failed.

**The configuration blob** (`components/core/alarm_core/src/codec.c`): header
`"ACFG" | format | compat | header_len | payload_len | crc32`, then items `id u16 | len u8 |
value`. An id is `(group << 8) | field` (group 0 system options and the code salt, 1 partitions,
2 zones, 3 users, 4 devices); field 0x00 of a group selects the 1-based entry. Unused zones,
empty user slots and blank devices are left out, so a typical panel stores a few KB (at most
`ALARM_CONFIG_BLOB_MAX`, about 32 KB with the P4 capacities). Integers accept any width up to
four bytes on read and are range-checked; unknown groups and fields are skipped; an entry index
beyond this build's capacities or a configuration `alarm_config_validate` rejects fails the
whole decode. Names are UTF-8, up to 47 bytes (docs/features/37): each is written
where firmware before then looks, cut on a whole character to its 23 bytes, and - when longer -
whole in a field such firmware skips (zone 0x0A, partition 0x06, user 0x05, device 0x05), so a
rollback keeps every name's beginning. A zone's spoken name (0x0B, eight little-endian u16
token IDs in exactly 16 bytes) and its voice revisions (zone 0x0C-0x0E, system 0x1E;
docs/features/08 voice-first-build.md D07) are written only when not zero. `compat` is raised only by a writer that older readers would misread. Field ids
are an external contract: never reuse or renumber one.

### Installer mode (engine: `components/core/alarm_core/src/program.c`)
`alarm_engine_program_enter/exit/keepalive/set_code`, `alarm_engine_recovery_left`; state
reported in the snapshot (`program`, `recovery_left_ms`). One installer session at a time
(another source gets `bad_state`). Arming and the walk test are refused (`bad_state`);
alarms, fire, CO and panics work as always. The engine ends installer mode itself after
`ALARM_PROGRAM_TIMEOUT_S` (15 min) without `keepalive`/`set_config`; the touchscreen sends a
keepalive every 30 s while the installer is touching it.

### Installer screens (touch, `components/ui/alarm_ui/src/ui_installer.c`)
Settings > About > Installer (the last row of About, BL-189). Locked: "Enter the installer code" (the screen
never shows the factory code's digits, BL-187), and during the recovery window "Forgot the code? Open without it (N s left)" with a
confirmation. Unlocked (status bar: "Installer · Arming off · Tap to leave"):

- **Delays and arming** (it was System options): exit time, entry times 1 and 2, report delay (abort window), siren time,
  fire siren timeout, auto-stay, exit beeps, arm without a code, arm remotely without a code,
  quick exit, chime needs a code, shortest new
  code, wrong codes before lockout and lockout time, come back armed after a power cut,
  installer code recovery, and every other system option ([07](../07-UI-LVGL-Pro/README.md)
  UI-041). Warnings as in CFG-008.
- **Zones**: list (with where each sensor is), add ("Where is the sensor?" first, M11), and per
  zone how it is right now (the source and what it promises), name, type (the numbered zone types),
  sensor kind, source, sensor settings (wired: contact and response time; supervision and
  unreachable after), night zone (interior types), chime, bypassable (24-hour types), report
  remotely, cross-zone partner, alarm limit (system setting, 1 to 6, or never), remove. A
  zone's sensor is stored at once, apart from the configuration ([22](../22-Zone-Sources/README.md)).
- **System name** (partition 1), **installer and master codes**, **run the setup again**,
  **factory reset** (hold to confirm, optionally with the event log).
- **Partitions** (in use, name, delays), **keypads and devices**, **Z-Wave** after Zones (in
  builds with Z-Wave, [36](../36-Z-Wave-Sensors-ZAC93/README.md)), and under This panel the
  Panel Manager switch (the web server), the installer's own browser sign-in and Developer mode
  ([31](../31-Browser-Panel-Manager/README.md)); then **Updates**: the update server (a domain, checked
  before it is kept; a server whose signing key is new asks the installer to read its fingerprint and
  hold) and the update channel, held while an update runs (OTA-020..OTA-022, [13](../13-OTA-Updates/README.md)).
  A zone's sensor settings and its tamper and
  low-battery inputs are set here too, and a user's partitions in Settings > Users with the
  installer code: everything the Panel Manager sets but certificate authorities and developer
  tools ([07](../07-UI-LVGL-Pro/README.md) UI-041).
- Wi-Fi and date and time moved out of installer mode to their own Settings sections (M5, see
  [Who can change what](#who-can-change-what)).

Pages edit a working copy; leaving a page applies the options, partitions, zones and devices of
the copy on top of the current configuration (so code changes made meanwhile are kept),
validated first. Leaving the Installer section or Settings, or 5 idle minutes, ends installer mode.

### Factory reset (`components/services/platform_esp/src/panel_system.c`)
`panel_system_factory_reset()`: `alarm_service_factory_reset()` logs `factory_reset`
(reported) and stops every further NVS write; the log is flushed (and cleared if asked);
`nvs_flash_erase()`; restart. The next start is a first boot: factory configuration and codes,
the setup wizard.

### The backup file (CFG-005, CFG-006)
`components/core/backup` (portable, host-tested) is the file; `components/services/backup_service`
makes and restores it on the panel; the Panel Manager offers it on Installer > Access and
maintenance (`POST` / `PUT /api/v1/backup`).

- **The file:** `"RABK"`, format 1, then the payload encrypted with AES-256-GCM under a key from
  PBKDF2-HMAC-SHA256 of the installer's password (8 to 64 printable characters, 200 000
  iterations, a fresh 16-byte salt and 12-byte nonce per file); the 44-byte header is the
  associated data. The payload is one record per NVS entry, as NVS stores it, plus when it was
  made, by which firmware, on which kind of panel. Downloaded as `panel-backup-<date>.rabk`.
- **What it holds** is a table (`backup_policy.c`), the same one a restore replaces by:
  `alarm/config` (not the runtime state), `zones`, `net` (not a change on trial), `ui`, `sys`,
  `mqtt` (not the panel ID, its discovery or the log position the broker has), `notify` (not
  the log positions), `ntfy`, `pushover`, `weblogin`, `zwave`, `zbpanel` (the panel's record
  of its Zigbee devices, docs/features/20 ZB-020), `rf`, `clock`, `automations` (what the house
  does by itself, docs/features/41 AUT-008), and from `ota` the update server (`trust`) and the
  channel (`chan`). A restore brings the server back without the signing key accepted for it:
  the installer checks the server and confirms its key's fingerprint again (docs/features/13
  OTA-020); until then a release signed with that key is refused. An older file's `ota`/`src`
  (the update source before the trust record) is accepted and left out, so a file without
  `trust` (or `chan`) puts the panel back on the default server (or channel). A namespace that can't
  be read makes no backup at all, never one without it. Never: `halink` (the panel's certificate and key, and the pairing made
  with it), `panelweb`, `devrem`, `devota` (the remote switches and the developer enrollment),
  `log`, `nettrial`, `touch`, ESP-IDF's own. The Zigbee stack's own network (its key, PAN and
  frame counter; docs/features/20) joins the table when its namespace is known on the bench.
- **Making one:** the installer signed in, with their password again unless typed in the last
  10 minutes (docs/features/31 WEB-016); logged
  as `backup_made`. The panel reads the entries and encrypts in the Panel Manager's worker; the
  key derivation takes seconds.
- **Restoring one:** the installer with a recent password as above, every partition disarmed
  and quiet.
  The whole file is decrypted and checked first (`backup_check`: the right kind of panel, only
  entries the table keeps, each once, a configuration the engine takes, bindings that decode);
  a refusal changes nothing. Then `backup_restored` is logged, the engine stops storing its
  state (as for a factory reset), every entry the table keeps is erased and the file's written,
  namespace by namespace, and the panel restarts two seconds after answering. A power cut in
  that fraction of a second leaves a mix: restore again. Entries the table doesn't keep stay as
  they are, so a replacement board keeps its own identity; with the old board's Z-Wave radio
  moved over, the restored keys and node cache are that radio's network again.

### Browser configuration (Feature 31)

Panel Manager serves the same implemented settings from the panel over authenticated HTTPS.
System-master and Installer roles retain their existing responsibilities. Alarm programming
requires the signed-in installer (read again on the engine task, CORE-PROG-011), an
unchanged configuration revision and an idle panel; `alarm_service_program_apply_as` performs
the complete check/apply/leave operation on the engine task. Browser user/code writes act as the
signed-in person, whose slot the engine reads again (CORE-USR-028). Secrets never
read back. Network edits need confirmation within 120 seconds or restore the saved profile,
including after a reboot. Local Installer owns the persistent Web server and Developer mode
switches; web-off disables both. See docs/features/31 for the field/action inventory and HIL evidence.

## Test and acceptance

- **Host tests** (`components/core/alarm_core/test/`): `test_codec.c` (CORE-CFG-012..017, with
  the frozen fixtures `codec_fixture_v1.h` and `codec_fixture_v2.h`), `test_config.c`
  (CORE-CFG-001..011 and 018; CFG-003's CORE-CFG-005), `test_program.c` (CORE-PROG-001..007),
  `test_users.c` (CORE-USR-012, CORE-USR-025: who manages whom) and `test_partitions.c`
  (CORE-USR-023).
- **UI preview** (`host/ui_preview/main.c`): the shots `installer_locked`, `installer_menu`,
  `installer_options`, `installer_zones`, `factory_reset`, `installer_recovery`,
  `recovery_confirm`, `recovery_new_code`, `installer_users`, `installer_partition`,
  `installer_device` and `installer_panic_keys` (CFG-007..009, CFG-011, UI-041).
- **In a browser on the PC:** `components/services/panel_web/test/panel_pages.test.cjs` opens
  every page as each role against `web/mock-panel.cjs`, which mirrors the access table
  (CFG-011; docs/features/31 WEB-016).
- **The backup file** (CFG-005, CFG-006): `components/core/backup/test/test_backup.c` (the format,
  the crypto against published vectors and Python's, the frozen format-1 file, the policy, the
  check before a restore) and `panel_pages.test.cjs` "backup" (the routes, their refusals and a
  round trip through the mock, which seals and opens with Node's crypto); on a panel,
  `backup check` proves the PSA port and times the key derivation.
- **Code review:** CFG-010, CFG-012 and CFG-013
  (`components/services/platform_esp/src/alarm_store.c`, `alarm_service.c`); their fault
  injections are docs/features/06's (P4-013, P4-014).
- **On a panel:** see [Tasks](#tasks).

## Open questions
- Allow remote configuration over MQTT (installer code required), or local only? Leaning local
  only in v1. As built: MQTT carries commands, never configuration: arm, disarm,
  bypass and unbypass, chime, quick exit and panics (`components/core/alarm_proto/src/commands.c`),
  and the switched Z-Wave devices (docs/features/10 MQTT-019, MQTT-020); configuration is changed
  at the panel or in the Panel Manager (docs/features/31). Related: the configuration exported
  over MQTT (BL-054), user management over MQTT (BL-075). Decided: local only -
  MQTT carries commands, never configuration; the backup is the Panel Manager's file (CFG-005).
- Export encryption: password-based (PBKDF2 + AES-GCM) or none (hashes only, no secrets)?
  Decided: always encrypted, password-based (PBKDF2 + AES-GCM), with the
  integrations' secrets and the Wi-Fi network in, so one file restores the whole panel; the
  installer alone downloads and restores it (CFG-005, CFG-006). The file holds the Z-Wave keys
  and the code hashes, and a short code's hash can be guessed offline, so it is never plain.

## Tasks

Each open box names its row in [the backlog](../../BACKLOG.md).

- [x] Versioned configuration blob + host tests with a frozen fixture (M4)
- [x] Installer mode in the engine + screens (M4)
- [x] Factory reset paths (touchscreen, console) and power-up recovery (M4)
- [x] Durable-write tracking with retry, and a start-up that never resets to factory defaults to
      recover from a failure it could not diagnose (CFG-010, -012, -013)
- [x] The Panel Manager follows the access table, the installer manages every user, and the
      installer screens set everything the browser sets (CFG-011, UI-041; e07106d)
- [x] The update server and the channel are the installer's: Installer > Updates and the Panel Manager's
      Software updates page; Software update is the installer's and the system master's (BL-186, OTA-022)
- [x] The installer code never arms or disarms ([03](../03-Users-Codes-and-Authority/README.md) USR-001, BL-187)
- [x] The backup file (CFG-005, CFG-006): codec and encryption with host tests, the Panel
      Manager's download and restore for the installer, all or nothing (BL-002, 9f8ab94)
- [ ] The backup file on a panel: `backup check` (the PSA port, the key derivation's time), a
      backup made and restored onto the same panel, then onto a replacement board with the
      Z-Wave radio moved over; the Zigbee network's namespace added to the policy once known
      (BL-002)
- [ ] Try the 2026-09-24 additions at a panel's screen: the installer's new pages (partitions,
      keypads and devices, a zone's sensor settings, a user's partitions; UI-041) and users
      managed by the installer and by the masters (BL-024)
- [ ] The same rules in the Panel Manager on a panel, signed in as each role: its refusals,
      nothing changing while armed, whoever manages a user setting their browser password
      (docs/features/31 WEB-016) (BL-080)

**Tracked elsewhere**
- The fault injections that prove CFG-012 and CFG-013: docs/features/06 ("Prove P4-013 and
  P4-014 with fault injection"; BL-032).
- Owed on a panel and tracked in the bug report: the Panel Manager's limits on guessing
  ([B005](../../bugs/B005-panel-manager-code-guessing.md)), BL-025.
