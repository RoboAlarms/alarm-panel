# Features

The design documents for people working on the code: one folder per feature, each `README.md`
following [`_TEMPLATE.md`](_TEMPLATE.md) (goal, reference behaviour, requirements with
IDs, design, interfaces, tests, open questions, tasks). The user and installer guides are a
separate thing: <https://docs.roboalarms.com>, source in the
[RoboAlarms/documentation](https://github.com/RoboAlarms/documentation) repository.
What is left to do, across every feature and in order, is the [backlog](../BACKLOG.md).

## Index and status

| # | Feature | Milestone | Status |
|---|---|---|---|
| 00 | [Architecture and roadmap](00-Architecture-and-Roadmap/README.md) | all | Living document: principles, component tiers, build options, the decision log (ADR-001..044) |
| 01 | [Core alarm engine](01-Core-Alarm-Engine/README.md) | M1, M4 | **Implemented v1**: host-tested, on a panel since 2026-09-19; options beyond v1 BL-089 |
| 02 | [Zones and sensor inputs](02-Zones-and-Sensor-Inputs/README.md) | M1 core / M11 sources | **Implemented v1**: the zone model and the source framework; left a wired contact (BL-029), Zigbee's radio (BL-036), keypad inputs (BL-048), EOL (BL-063) |
| 03 | [Users, codes and authority](03-Users-Codes-and-Authority/README.md) | M1 core / M4 UI | **Implemented v1** (core + touch UI); the installer's user pages on a panel BL-024 |
| 04 | [Keypad behaviour and commands](04-Keypad-Behavior-and-Commands/README.md) | M1 parser / M3 UI | **Implemented v1** (parser + touch keypad); left remote keypads (BL-048) |
| 05 | [Event log](05-Event-Log/README.md) | M4, M5 | **Implemented v1** (flash ring, viewer, MQTT replay); left the bench checks (BL-095), export over MQTT (BL-054) |
| 06 | [CrowPanel P4 platform (BSP)](06-CrowPanel-P4-Platform/README.md) | M2 | **Implemented v1**: on a CrowPanel 7" V1.1 (P4 v1.3) since 2026-09-19; the v3 image never run; hardware acceptance BL-032; the 10.1" V1.2, the same board with a bigger screen, runs the same firmware with the same features (2026-10-10; its update channel and tools BL-209) |
| 07 | [UI with LVGL Pro](07-UI-LVGL-Pro/README.md) | M3-M6 | **Implemented v1**: in daily use on a panel since 2026-09-19; later changes to check on a panel (BL-024), BL-059, BL-060; About holds System details and Installer, the update server and an update's progress with Stop are built and shown in the preview, not yet tried on a panel (BL-186, BL-188) |
| 08 | [Audio and annunciation](08-Audio-and-Annunciation/README.md) | M2/M3 tones; A0-A5 voice after Z-Wave | **In progress**: tones done (listening checks BL-032); the voice's first build is built (2026-10-09): spoken zone names from the supplied en-US words, chosen on the touchscreen, with the chime, walk-test and status announcements and the Chime, Voice and Beeps volumes; its pack (each word spoken alone, 60 ms apart, made louder on the panel) chosen by ear on a panel (2026-10-10); the listening check of the announcements is next (BL-202); the rest of the voice BL-015, BL-044, BL-203 to BL-205 |
| 09 | [Wi-Fi, time and provisioning](09-WiFi-Time-and-Provisioning/README.md) | M2/M4 | **Implemented v1**: Wi-Fi, time, setup and the connection check on a panel; left BL-032, the role step and keypads' mDNS (BL-048) |
| 10 | [MQTT and Home Assistant](10-MQTT-and-Home-Assistant/README.md) | M5 | **Implemented v1**: host-tested; part 1 on a panel with Mosquitto and Home Assistant passed 2026-09-29, the code scenarios and fault injections left (BL-026), replay after a broker restart BL-112; export and backup over MQTT BL-054 |
| 11 | [Panel link: master and keypads](11-Panel-Link-Master-Keypad/README.md) | M7 | **Planned** (BL-048; link keypads 1-8) |
| 12 | [C6 co-processor firmware](12-C6-Coprocessor-Firmware/README.md) | M6/M7 | **In progress**: the shipped C6 carries Wi-Fi; our own C6 build and bridge BL-048, runtime recovery BL-097, recovery doc BL-072 |
| 13 | [OTA updates](13-OTA-Updates/README.md) | M6/M8 | **Implemented v1**: signed updates with rollback verified on a panel; updates from the official update server, with progress and Stop, tried on a panel against a local server (left: BL-186, BL-188; findings BL-197 to BL-201); the reviewed self-test on the bench BL-023; releases BL-050, SD BL-061, keypad relay BL-048 |
| 14 | [Configuration and installer programming](14-Configuration-and-Installer-Programming/README.md) | M4, M5 | **Implemented v1**: installer mode, access table, factory reset, the encrypted backup file (built 2026-09-29; on a panel: BL-002) |
| 15 | [Security and hardening](15-Security-and-Hardening/README.md) | all | **In progress**: signed updates, PBKDF2 code hashes, the release profile; the hardened profile and the rest BL-049, repository settings BL-098 |
| 16 | [Outputs, siren and relays](16-Outputs-Siren-and-Relays/README.md) | M1 core / later HW | **In progress**: logical outputs in the engine; hardware, drivers and rules BL-037 |
| 17 | [Power and supervision](17-Power-and-Supervision/README.md) | M2+ | **In progress**: power restore, the STC reading, the battery icon and the reset reason; battery acceptance BL-038, task watchdog BL-068 |
| 18 | [Schedules and auto-arm](18-Schedules-and-Auto-Arm/README.md) | later | **Planned** (BL-046) |
| 19 | [4" ESP32 panel (E32R40T, "CYD")](19-CYD-ESP32-Port/README.md) | M9 | **On hold** (2026-09-24): out of CI and the Flasher until it resumes; the UI needs about 3x its LVGL memory (BL-079) |
| 20 | [Zigbee sensors (ESP32-H2 radio)](20-ESP32-H2-Sensor-Radio/README.md) | M10 | **In progress**: device management, the touchscreen's and the Panel Manager's pages, sirens, keypads and key fobs built and host-tested on the PC (simulator and preview); the coordinator and the H2's RCP firmware build (in every P4 image, ADR-047); on a panel's H2 the stack starts, survives losing the radio and opens the add window; a real sensor and the rest of the bench still owed (BL-036) |
| 21 | [Testing and CI](21-Testing-and-CI/README.md) | all | Living document: host tests with sanitizers, UI preview, simulator, CI checks; left BL-048, BL-077 |
| 22 | [Zone sources](22-Zone-Sources/README.md) | M11 | **Implemented v1**: `zone_hub` with the simulator and wired sources on a panel since 2026-09-19; left BL-027, BL-029, BL-062, BL-063 |
| 23 | [Home Assistant integration (HACS)](23-Home-Assistant-Integration/README.md) | M12, M13 | **In progress**: M12 released (HACS testing) and paired 2026-09-20; M13 on a panel with a test Home Assistant 2026-10-06 (the link's 95 s drop, B029, and the other findings fixed; the integration's 0.2.0 candidate committed on its release branch and green in GitHub CI, its release waiting for the maintainers, BL-173); the panel's Z-Wave devices in the integration (2026-09-29); BL-027 (parts 1 and 2 passed), BL-040, BL-045, BL-171 |
| 24 | [Devices (home control)](24-Home-Control/README.md) | M13 | **In progress**: the device registry, the Home Assistant device source and the Smart Home screen built, every kind of device tried both ways with Home Assistant on a panel 2026-10-06, with their values, what each can do, Arrange's drag and the user acceptance run's fixes; the approved words built and tried on a panel 2026-10-07; a thermostat's resting words (BL-171), a readable offline tile (BL-181) and long names kept in their tiles (BL-184) built and checked in the preview 2026-10-07; still to see on a panel: flicker, "didn't turn on", an earlier date's "Offline since" and those three (BL-183); what BL-171 left (a cover's class, presets, Z-Wave values); Settings > Devices, events BL-045 |
| 25 | [RF sensors 315/345/433 MHz](25-RF-Sensors-315-345-433/README.md) | M14 | **In progress**: 433 MHz through a module or a CC1101 on a panel; the sensors people buy tested as mock devices, B025 fixed in code (BL-163); bench fixtures BL-022, more families BL-039 |
| 26 | [Notifications](26-Notifications/README.md) | M15 | **Implemented v1**: ntfy delivered on a panel 2026-09-21; Pushover BL-028, store-and-forward BL-026, B006 BL-025 |
| 27 | [ntfy](27-ntfy/README.md) | M15 | **Implemented v1**: delivered to ntfy.sh on a panel 2026-09-21; a self-hosted server's own authority BL-058 |
| 28 | [Pushover](28-Pushover/README.md) | M15 | **Implemented v1**: built, not yet run against Pushover (BL-028); quota warning and QR written, unseen against the real service |
| 29 | [Storage and SD card](29-Storage-and-SD-Card/README.md) | M16 | **Implemented v1**: FAT mount, Storage page, format; read-only checks on a panel; destructive card tests BL-034 |
| 30 | [Remote development, network flashing and screen control](30-Remote-Development/README.md) | M17 | **Implemented v1**: network/USB updates with probation and rollback, diagnostics, the screen; on a P4 since 2026-09-21; leftovers BL-035 |
| 31 | [Browser Panel Manager](31-Browser-Panel-Manager/README.md) | M18 | **Implemented v1**: the Panel Manager on a P4 since 2026-09-22; the Overview as the alarm command centre (arm and disarm without a code) built 2026-09-29, a panel's checks BL-113; the zone page, People and codes, Troubles and no keypad codes built 2026-10-02, a panel's checks BL-138; the Software updates page with the update server, progress and Stop built 2026-10-08 (BL-186, BL-188); checks on a panel BL-080, BL-035; BL-064, BL-076 |
| 32 | [Documentation and brand](32-Documentation-and-Brand/README.md) | docs | **Implemented v2**: guides edition 02 (2026-09-25), now in RoboAlarms/documentation; review and publishing BL-017, Z-Wave BL-071, Spanish BL-047 |
| 33 | [Modular architecture and board support](33-Modular-Architecture-and-Boards/README.md) | M9 | **Implemented v1** (steps 1-10, 2026-09-23); the boards on one main board share a family library, and a firmware can be built on another (BL-209); left BL-079, BL-087 |
| 34 | [Flasher and bench tool (desktop)](34-Flasher-and-Bench-Tool/README.md) | M19 | **Implemented v1**: the Flasher host-tested and walked against `--mock`, with guided wiring before the panel is plugged in (2026-09-29); the CrowPanel 10.1" offered beside the 7", the person naming the board when its chip can't (2026-10-10, BL-209); voice packs installed from its bundles (2026-10-10, BL-204); the firmware from the official update server, none built in (2026-10-12, FLS-032); BL-033, BL-041, BL-051, BL-078, BL-081, BL-086, BL-117 |
| 35 | [Battery-backed clock (DS3231 module)](35-Battery-Backed-Clock/README.md) | M20 | **Implemented v1**: a module on a panel since 2026-09-25 (bench steps 1-2); BL-030, BL-033, BL-050 |
| 36 | [Z-Wave sensors (Zooz ZAC93)](36-Z-Wave-Sensors-ZAC93/README.md) | own track Z1-Z11 | **In progress** (own track Z1-Z11): built and host-tested through Z10 and most of Z11, and aligned with Z-Wave JS (ZD-4: the 2026-10-03 audit's fixes, BL-152); a ZAC93 passed the no-sensor bench; left: real devices on a panel (BL-153, BL-019..021), Z-Wave JS's features not built yet (BL-162), BL-042 |
| 37 | [Globalization (languages and regional formats)](37-Globalization/README.md) | M22 | **In progress**: G1 and G2 done in software (2026-09-28, -29): every word through the catalog, English only; names in any Latin script, 48 bytes, the accent layer. G3 done in software (2026-09-29): Language and region, changed at once on every screen, dates, the clock and numbers by region, test languages in the preview, every Panel Manager word through the catalog the panel serves. G4 done in software as a draft (2026-09-29): Spanish on every screen, in notifications and in the Home Assistant integration, left out of release builds; British English (`en-GB`) drafted the same day; next their reviews and a panel (BL-005, BL-110); the reference-behaviour check (BL-070). Language packs (2026-09-30, ADR-045): every language but English is a signed file installed after the firmware (the Flasher, the Panel Manager, the release manifest), so the P4 image carries no translation; built and tested on the PC, on a panel next (BL-121) |
| 39 | [Optimizations (firmware size, storage and leftovers)](39-Optimizations/README.md) | M23 | **In progress**: the P4 image audited without giving up a feature, Z-Wave included (F1-F19); the cheap part done 2026-10-02 (build settings, LVGL's theme and default font, 13 TLS options, icons per size, minified pages, compact generated screens): the v1 default image 65.4 % -> 58.4 % of its slot; the letters, the saver photo and the full certificate list as signed asset packs (ADR-046, 2026-10-10): the release image 73.7 % -> 66.8 %; left: panel checks, the biggest builders, then the leftovers; the web pack held back until more space is needed; BL-122..126 |
| 38 | [Wording and design](38-Wording-and-Design/README.md) | - | **Future, not well defined yet** (2026-09-29): two rounds of research, the draft rules and the Overview mockup written up; the Panel Manager's Overview built to them (BL-113); the rest BL-114 |
| 40 | [Walk test](40-Walk-Test/README.md) | M24 | **Built** (2026-10-01): the list of tested zones at the panel (Settings > Walk test, Home) and on a phone in the Panel Manager, with beeps and spoken names on the phone; next: live updates (BL-129), signal per radio sensor, the panel's spoken names with voice, two phones on a panel (BL-128) |
| 41 | [Automations](41-Automations/README.md) | M25 | **Building** (2026-10-06): one trigger and one or two actions, run by the panel; they may arm, disarm, unlock and open behind a plain confirm and an installer option; phases A (device automations, BL-168), B (the Z-Wave device source, BL-169) and C (arming and disarming, BL-170) built and host-tested; the Z-Wave outputs' Smart Home tiles to draw, the bench next |

## Roadmap

| Milestone | Scope | Depends on | Status |
|---|---|---|---|
| **M0** | Repo skeleton, feature plans | - | Done |
| **M1** | `alarm_core` v1 + host tests + CI | M0 | Done |
| **M2** | CrowPanel bring-up: ESP-IDF v6.1 project, BSP (EK79007, GT911, backlight, audio), LVGL, Wi-Fi via C6, SNTP, NVS, alarm task, serial-console zone simulator | ESP-IDF v6.1 installed; P4 chip revision known | Done: on a panel since 2026-09-19 |
| **M3** | LVGL Pro UI v1 (master role, one partition), sounder | M2 | Done |
| **M4** | Installer/config UI, event log, first-boot wizard, user management UI | M3 | Done |
| **M5** | MQTT over TLS + Home Assistant discovery | M2 | Built; a real broker still to try (BL-026) |
| **M6** | Signed OTA with rollback for the P4, C6 co-processor OTA | M2 | Done: verified on a panel |
| **M7** | Panel link: protocol + LAN transport + PC virtual keypad, then the custom C6 ESP-NOW bridge and the P4 keypad role | M3, M6 | Planned (BL-048) |
| **M8** | Keypad OTA relay | M6, M7 | Planned (BL-048) |
| **M9** | CYD (ESP32) port | M7 | On hold (BL-079); its prerequisite, docs/features/33, done |
| **M10** | Zigbee sensors: P4 coordinator with an ESP32-H2 radio co-processor, IAS zones (docs/features/20) | M11, H2 hardware | In progress: device management, the pages, sirens, keypads and key fobs built on the PC, and the firmware built; a real sensor and the bench acceptance still owed (BL-036) |
| **M11** | Zone source framework (`zone_hub`), wired and simulator sources, zone editor source picker (docs/features/22) | M4 | Done; a wired contact still to fit (BL-029) |
| **M12** | Home Assistant integration from HACS: mDNS discovery, pairing on the panel, local TLS API, entities and commands (docs/features/23) | M5 | Done: HACS testing release 2026-09-20; acceptance BL-027 |
| **M13** | Two-way Home Assistant: HA entities as zones (search on the panel) and the Devices screen for lights, locks, climate, covers; native Zigbee devices after M10 (docs/features/22, 24) | M11, M12 | In progress: HA entities as zones and the Smart Home screen built; acceptance on a panel BL-045 |
| **M14** | RF receiver 315/345/433 MHz: EV1527, later 345 MHz and 319.5 MHz security-sensor decoders (docs/features/25) | M11, receiver hardware | In progress: 433 MHz on a panel; more families BL-039 |
| **M15** | Push notifications straight to a phone: the shared words and flood gate, ntfy (hosted or your own) and Pushover with its repeat-until-answered alarms (docs/features/26-28) | M5 | Built: ntfy delivered; Pushover to try (BL-028) |
| **M16** | Optional SD storage foundation, format safeguards, internal-flash accounting and About organization (docs/features/29) | M2, M4, M6 | Done; destructive card tests BL-034 |
| **M17** | Remote development: explicit dev builds, PC tool, authenticated network flashing/discovery/logs, then screen control and multiple hardware adapters (docs/features/30) | M2, M3, M6; coordinate docs/features/15, /21, /23 | Done (v1); leftovers BL-035 |
| **M18** | Panel-hosted browser management, full-resolution assistance, configuration parity, named helper permissions and runtime developer sessions (docs/features/31) | M4, M6, M17; coordinate docs/features/14, /15, /21 | Done (v1) |
| **M19** | Desktop flasher and bench tool for people who don't build firmware: port list, board detection with override, backup, install/update, console and 433 MHz receiver test; the receiver's data pin as a setting (docs/features/34, /25) | M6, M14, M9 | Built; the redesign on a panel BL-033, distribution BL-041 |
| **M20** | Battery-backed clock: an optional DS3231 on the board's I2C connector keeps the time through power cuts; the time by hand at the panel, in the Panel Manager and from the Flasher (docs/features/35) | M4, M18, M19 | Built; the rest of the bench BL-030 |
| **M21** | Z-Wave sensors on the panel: a Zooz ZAC93 on the radio socket or J7, chosen on the console or in the Flasher's Connected parts; in every P4 firmware with the radio off until wired; its own track Z1-Z11 goes on (docs/features/36) | M11, M18, M19, a ZAC93 | In progress on its own track, Z1-Z11 (BL-019..021, BL-042) |
| **M22** | Globalization: every word through one catalog per language, names in any script, a language and region setting, then Spanish, Chinese, Japanese, Korean, Arabic and Hebrew; phases G0-G7 (docs/features/37) | M3, M4, M15, M18 | In progress: G1, G2 and G3 done in software (2026-09-28, -29); G4 Spanish drafted (2026-09-29), its review and a panel next (BL-005) |
| **M23** | Optimizations: the firmware smaller without giving up a feature (Z-Wave stays in every image): a size report, build settings, data stored better, asset packs in the pack store, our own code, then the leftovers features left behind (docs/features/39) | M22 (language packs) | In progress: audit done 2026-09-30 |
| **M24** | Walk test: the tested zones listed at the panel and on a phone, the Panel Manager's live updates over a WebSocket, signal per radio sensor, then the panel's spoken zone names with voice (docs/features/40, /31) | M3, M18; voice (08) for its last step | Planned (BL-128, BL-129) |
| **M25** | Automations: device automations on the touchscreen and in the Panel Manager (A), the Z-Wave device source (B), arming by automation with its warning (C) (docs/features/41) | M13, M21 | Built and host-tested; the bench next (BL-168, BL-170) |

Progress on each milestone is in the index above and in the status line of each feature; the
work still to do, in order, is in the [backlog](../BACKLOG.md).

Globalization (docs/features/37, M22) is the main milestone under way (decided 2026-09-27; G1 done 2026-09-28): text is cheaper
to wrap now than after more features, names outside ASCII can't be drawn today, and the voice packs
take their locale IDs from it. From its G1 on, new strings go through the catalog.

Annunciation voice (docs/features/08): en-US words and phrases recorded on a computer with a
text-to-speech model and shipped compressed in flash, plus imported files; nothing is synthesised
on the panel (ADR-052). A0, the voice and the word list, started 2026-10-08 (BL-015). The first
build, spoken zone names from that library (BL-202, voice-first-build.md), is built and waits for its
listening check on a panel; custom recordings and the rest follow the Z-Wave completion
checkpoint (BL-044).

Z-Wave sensors (docs/features/36) are a separate project on their own milestone track, Z1-Z11; they
touch the main project only at their integration milestones (Z6 onward). Reaching the panel (Z9)
gave them their entry here, M21; the track's own milestones stay in docs/features/36.

## Current status (2026-10-13)

This is development software and **not a listed or certified alarm system**. "On a panel" below
means a CrowPanel Advanced, 7" or 10.1" (the same board with a different screen size, P4 silicon
v1.3); firmware for P4 silicon v3 builds but has not run on a v3 board. Everything still to do, in order and with what it needs, is the
[backlog](../BACKLOG.md).

- **Working on a panel:** the alarm engine, event log, touch UI and screen saver, sounder, Wi-Fi
  and time, signed updates with rollback, the simulator and wired zone sources, 433 MHz through a
  receiver module or a CC1101, SD storage, the Browser Panel Manager, remote development, a
  DS3231 clock module and ntfy notifications (01-09, 13, 22, 25-27, 29-31, 35). MQTT with a broker
  and Home Assistant discovery passed its first part (10); the Home Assistant integration pairs,
  arms and disarms both ways and binds Home Assistant sensors as zones (23); the Smart Home screen
  has driven every kind of Home Assistant device both ways (24). Their remaining checks are in
  the backlog (BL-026, BL-027, BL-183).
- **Built and host-tested, not yet tried on a panel (or only in part):** updates from the official
  update server with progress and Stop (13; a few checks left, BL-186, BL-188), the Panel
  Manager's Overview and redesigned pages (31), backup and restore (14), language packs (37,
  BL-121), the walk test (40), automations (41), the Flasher's guided wiring (34), Pushover (28),
  and the voice's first build, spoken zone names (08; its listening check is next, BL-202).
- **Zigbee (20):** devices, pages, sirens, keypads and key fobs are built and host-tested, and the
  coordinator is in every P4 image; on a panel's ESP32-H2 the stack starts, survives losing the
  radio and opens the add window. A real sensor and the rest of the bench are still owed (BL-036).
- **Z-Wave (36, its own track Z1-Z11):** in every P4 firmware with the radio off until a place is
  chosen; built and host-tested through Z10 and most of Z11 and aligned with Z-Wave JS; a ZAC93
  passed the no-sensor bench. Sensors as zones, keypads, switches and garage doors, SmartStart,
  network care and device and radio updates are not yet tried with real devices.
- **Languages (37, M22):** every word of the touchscreen, the event sentences and the Panel Manager
  goes through one catalog; names in any Latin script; Settings > Time and language changes every
  screen at once, with dates, the clock and numbers by region. Every language but English is a
  signed language pack installed after the firmware, so the P4 image carries no translation; a
  missing or damaged pack means English. Spanish and British English are drafted, left out of
  release builds until a native speaker has reviewed them (BL-005, BL-110).
- **Firmware size (39):** audited without giving up a feature; the letters English does not use,
  the saver photo and the full certificate list moved to asset packs, and the release image went
  from 73.7 % to 66.8 % of its slot.
- **Planned:** automatic updates (designed, BL-190), the panel link and keypads (11), schedules
  (18), outputs and siren hardware (16), more of the voice (08) and more languages (37, G5-G7).
  The 4" E32R40T port is on hold (19).
- **Owed on hardware:** the checks in the backlog's sections 1 and 3.
- **Known problems** and their fixes are written up in [`docs/bugs/`](../bugs/README.md).

## Conventions

- Requirement IDs look like `CORE-EXIT-003` or `MQTT-012`. Tests reference them in a comment.
- **Status** values: *Planned*, *In progress*, *Implemented vN*, *Future* (not scheduled yet).
- Anything that changes an external contract (MQTT topics, link frames, config schema) bumps a
  version and is recorded in the feature's README.
