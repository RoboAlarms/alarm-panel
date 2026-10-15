# Automatic updates (BL-190): proposed design

**Status:** proposed 2026-10-08; waiting for the maintainers' answers to section 6. Nothing here is built. It builds on BL-186 to BL-189 (approved 2026-10-07: the update server, the installer code, update progress, About). It replaces the old text of BL-190 and answers BL-191.

All requirement numbers are drafts: OTA-032..049, CORE-PWR-008, CORE-PROG-013 and ADR-051. Line numbers are those of 2026-10-08, after the BL-186/187 commits.

## Summary

Automatic updates would work in three separate steps.

1. **Checking** stays automatic. It gets random spread, so a power cut doesn't make every panel ask at the same moment.
2. **Downloading** becomes automatic on every panel. The verified image waits in the inactive slot, kept across restarts, while the alarm keeps working.
3. **Installing** means a restart, and while it lasts burglary, fire and CO detection are off. It stays a person's choice. The default stays *Ask me first*, and no update ever changes that. The system master, and only the system master, can choose *Important fixes by themselves* or *All updates by themselves*.

An automatic install happens only when all of these hold:
- It falls inside a daytime window the master picks: 10 AM-12 PM or 1-3 PM.
- Everything is disarmed and quiet by a stricter test than today's: no alarm memory, no fire check running, no fire, CO or 24-hour activity or trouble, nobody at the panel for 10 minutes, no arming automation due, and mains power not known to be off.
- The project has signed, offline, a release statement that covers that exact image.
- The panel has not already restarted by itself in the past 24 hours.

A house that is armed through the window just waits for the next day. There is no deadline. The update server gains a staged rollout, a pause and a "never by itself" flag. Home Assistant sees the plan but can't change it.

Before the first automatic install ships, four things in today's code must change. Each makes every restart worse than the "about a minute" the screen promises:
- The 60 s settle after boot drops short fire, CO and panic signals.
- Every restart opens a 120 s way into installer mode with no code.
- The self-test doesn't check the sounder, the reporting or the radios.
- A failed self-test restarts at once, even during exit time or a fire alarm.

The gap must also be measured on a panel, and the bench runs still owed must pass.

## 1. Where we start

- **Nothing installs by itself.**
  - A download starts only on Install (`components/services/panel_ota/src/panel_ota.c:1448-1450`). OTA-024 says so and points to BL-190 (`docs/features/13-OTA-Updates/README.md:49`).
  - It was decided on 2026-09-28: "no - a person asks" (`:377`).
- **Checks:**
  - The first check runs 3 min after boot (`panel_ota.c:61`, `:1396`). After that, one every 24 h of uptime, with no randomness (`:1418-1428`).
  - A failed check is tried again after 10 min, doubling up to 6 h (OTA-015, README:40).
- **The install gate** checks only four things: installer mode, armed, a bell or sounder, and a walk test (`components/core/alarm_proto/src/fw.c:459-483`).
  - The gate is asked again just before the slot switch (`panel_ota.c:845-897`).
  - The panel restarts at the first quiet second. Bench run 4 restarted 1 s after a disarm (README:314).
- **A waiting image is kept in RAM only** (`panel_ota.c:126`). The approved BL-188 says a power cut forgets it (`docs/features/07-UI-LVGL-Pro/mockup/screens/updates-run.js`).
- **A version that rolled back is offered again.**
  - `available` comes from the version comparison alone (`panel_ota.c:1007`).
  - `ota/rb` holds one 8-byte key: the start of the app's ELF SHA-256, XORed with the slot address. It exists only so the rollback is reported once (`panel_ota.c:1234-1251`).
- **Every restart costs more than the boot.**
  - For 60 s the engine ignores zone alarms (`components/core/alarm_core/src/config.c:44`; set on every start, `engine.c:273-275`).
  - A fault that restores inside those 60 s never alarms. When they end, only zones still faulted act (`zones.c:453-469`, `:512-526`).
  - A verified-fire zone then starts a 6 s smoke reset and a 90 s window (`zones.c:231-242`, `config.c:34-35`).
  - Every start also opens 120 s of installer mode with no code (`config.c:55`, `engine.c:276-278`, `include/alarm/alarm_engine.h:349-356`). The engine already knows why it started (`components/services/platform_esp/src/alarm_service.c:262-279`).
- **What the self-test checks:**
  - Five heartbeats: engine, log, screen, sensor-input processing and touch (`components/services/panel_ota/include/panel_ota.h:144-152`). It never checks the sounder, the reporting or the radios.
  - A failure restarts at once, armed or not (`panel_ota.c:1302-1303`; OTA-031, README:56).
  - A panel caught in exit time comes back armed, with no exit time (CORE-PWR-001/002, `docs/features/01-Core-Alarm-Engine/README.md:318-319`).
- **MQTT before a restart:**
  - The panel logs and waits 500 ms (`panel_ota.c:788-793`).
  - "offline" is published only when the client stops cleanly (`components/services/net_mqtt/src/net_mqtt.c:1111-1118`), or later as the last will, with a 30 s keepalive (`:1179-1184`).
- **Home Assistant's Install** is the payload `install` on `…/firmware/set`. It calls install with no user attached (`net_mqtt.c:1026-1027`; advertised at `components/core/alarm_proto/src/discovery.c:249-251`).
- **The Wi-Fi module (C6):**
  - It updates by itself once per image version, for any version that merely differs (`cp_ota.c:108-116`, `:220-267`).
  - Its image header is read, but the signature block is never verified (`cp_ota.c:84-96`).
  - It restarts with no second gate check (`panel_ota.c:795-803`). The approved BL-188 adds that check (`docs/features/07-UI-LVGL-Pro/mockup/screens/updates-run.js`).
  - It has never run (README:3-4).
- **Nothing about the offer is signed.**
  - The manifest, and now the check answer, are unsigned (OTA-002, README:27). ADR-023 reasoned "the manifest needs no second key" for a panel where a person presses Install (`docs/features/00-Architecture-and-Roadmap/README.md:184`).
  - The channel is a label the server keeps, and a withdrawn release's files stay on disk.
  - The server offers the newest candidate and sets `critical` when *any* candidate is critical.
- **One key signs every build.**
  - Release and development builds use the same key (`firmware/crowpanel_p4/sdkconfig.defaults:20`, `tools/idf.ps1:54-55`).
  - The build tooling can copy it to other computers. The recorded decision says the key never leaves the maintainer's PC (README:378-379).
  - Without hardware Secure Boot, only the first public key in the running app's signature block checks updates ([Espressif, "Secure Boot v2 - ESP32-P4"](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/security/secure-boot-v2.html)). OTA-001 burns no eFuses (README:26).
- **Notifications:** `firmware_update` events fall to the System category (`components/core/notify/src/notify_words.c:1165`), which is off by default (NOTE-037, `docs/features/26-Notifications/README.md:65`; `components/core/notify/include/notify/notify_gate.h:56-58`).
- **The update server:**
  - Its answer has no rollout, pause or auto-install field.
  - It can't tell a real panel's check from a made-up one.

## 2. Must be done before any automatic install ships

P1-P3 and P7 also fix today's person-started installs.

| # | What | Why (source) |
|---|---|---|
| P1 | **Settle (CORE-PWR-008).** During any settle, latch a fault on a fire, CO, 24-hour or panic zone from a radio sensor, so a fault then a restore still alarms when the settle ends. After a planned software start (`ALARM_START_SOFTWARE` after an `installing` or `rolling_back` record), use a separate settle, measured on the bench, for wired loops only. Measure `esp_restart()` to the end of the settle and to MQTT reconnect on a CrowPanel, and put that figure (N) in the words. | Short events inside the 60 s are lost today, not delayed (`zones.c:453-469`, `:512-526`; PWR-002, `docs/features/17-Power-and-Supervision/README.md:47`). "About a minute" is wording only (`components/ui/alarm_ui/src/ui_update.c:92`, `:329`). |
| P2 | **No recovery after a software start (CORE-PROG-013).** Open `recovery_until` only for `ALARM_START_POWER_ON` (and optionally BROWNOUT and EXTERNAL), never for SOFTWARE, PANIC or WATCHDOG. | `engine.c:276-278` ignores the reason. Recovery works only when everything is disarmed (`alarm_engine.h:351-356`), which an automatic install guarantees. While installer mode is on, nothing can be armed, and codes and zones can be changed. The screen's own words are "right after power-up only" (`components/ui/alarm_ui/src/ui_installer.c:100`). |
| P3 | **Gate (OTA-004, every install).** Refuse while any zone is `ALARM_ZS_VERIFYING`, or is `ALARM_ZS_ALARM` with the zone still faulted. Reasons: "A fire alarm is being checked" and "A sensor is still in alarm". Add tests in `test_fw.c`. | The gate checks only installer mode, the state, the bell, the sounder and the walk test (`fw.c:459-483`). Verification sounds nothing (`zones.c:231-242`), and verification state isn't stored (`include/alarm/alarm_config.h:107-128`), so a restart throws away the first trip. |
| P4 | **Self-test (OTA-046, OTA-031).** Check the outputs, and hold the rollback during exit or entry time and alarms (section 4). | Five heartbeats only (`panel_ota.h:144-152`). Rollback with no gate (`panel_ota.c:1302-1303`). |
| P5 | **After confirmation (OTA-047).** Keep the previous image for 7 days, count crash restarts, and send PANIC and WATCHDOG starts to Trouble. | Once the image is confirmed there is no way back (`panel_ota.c:1272-1287`). Abnormal starts go to System, which is off (`notify_words.c:1165`). |
| P6 | **Signed release statements, and a release key separate from the development key** (D5, Q5, Q6). | The section 1 facts on the unsigned offer and the single key. |
| P7 | **"offline" on MQTT before every restart (OTA-038).** | Section 1, MQTT before a restart. |
| P8 | **Bench runs:** 8 and 9, and 11 with a real mains cut (README:349, :351, :6-7). New runs: arming during the 2-minute notice; a fire-verification trip at the chosen minute; a rollback hold with the `no_touch` bench fault image during exit time and during an alarm; arming while the Wi-Fi module's image streams. All on a CrowPanel. | Runs 8 and 9 are "Not run ... owed"; "A real power cut is still owed". |

## 3. Decisions

### D1. Three steps, each decided on its own
**Recommendation:**
- Checking: automatic, with spread (D6).
- Download and verify: automatic on every panel, whatever the mode, within D5's limits.
- Install (the restart): a setting (D2).

**Why:**
- Downloading writes the inactive slot while the alarm keeps working (OTA-003, README:28).
- With the image already downloaded, an Install no longer waits for the download.
- [ETSI EN 303 645 V3.1.3](https://www.etsi.org/deliver/etsi_en/303600_303699/303645/03.01.03_60/en_303645v030103p.pdf), 5.9-3, gives two examples: devices reconnect after a randomized delay, and the maker notifies devices "in batches".

**This changes approved screens.** In BL-188 the download is step 1 after Install (`docs/features/07-UI-LVGL-Pro/mockup/screens/updates-run.js`), and Stop throws the download away (`:108`). See Q2.

**Rejected:**
- One switch for all three steps, as Shelly has ([Shelly, "Updating Firmware on a Shelly Device"](https://shellyusa.atlassian.net/wiki/spaces/SU/pages/559153188/Updating+Firmware+on+a+Shelly+Device)).
- Never downloading by itself, as ESPHome does ([ESPHome, "Managed Updates via HTTP Request"](https://esphome.io/components/update/http_request/)).

### D2. Modes, default and who
**Recommendation:**
- **Modes:**
  - *Ask me first* is the default, for new panels and for panels that update to this firmware. An update never raises the mode.
  - *Important fixes by themselves*.
  - *All updates by themselves*.
  - A seller can build another default with the Kconfig option `PANEL_OTA_AUTO_DEFAULT`.
- **"Important"** means one of three things:
  - the release's signed statement marks it critical;
  - it is a stepping stone the statement names toward a critical release;
  - the running version is on the signed withdrawn list.

  In this mode the panel's check sends `"want": "critical"`, and the server offers the oldest critical release the panel can reach, not the newest feature release. Today the server picks the newest candidate and flags it when any candidate is critical, and the panel keeps one `critical` flag for the offered version (`panel_ota.h:138`). So a panel on 0.8.0, with a critical 0.8.1 and a feature release 0.9.0 published, would install 0.9.0 by itself. The project ships security fixes as patch releases on the current line.
- **Who:**
  - The **system master alone** sets *Install updates* and *When*: with their code at the panel, with their password in the Panel Manager.
  - The **installer** sees both read-only, can always set the mode back to *Ask me first*, and keeps Install, Not today and Stop (OTA-022, README:47).
  - Partition masters, standard users and guests see only "Update" or "Updating" (BL-189).
  - Every change is logged with who made it.
- **Back to *Ask me first*** when:
  - the update server or its accepted key changes;
  - the channel changes to anything but Stable;
  - a restore brings a different server or key.

  Software update then shows one row until the master opens it: "Automatic installs are off: the update source changed". The notice goes to Trouble (D7).
- **Not with factory codes:** raising the mode is refused while the master or installer code is still the factory one, as OTA-021 does for servers (README:46).
- **Dev:**
  - On the Dev channel the row shows *Ask me first*, dimmed, with "This panel gets special builds: each one asks first".
  - On a development address, or any `http://` base URL, there is no automatic download and no automatic install, whatever the mode. The console address takes its keys "without a check" (`panel_ota.h:258-260`), and it may be plain HTTP (OTA-017, README:42).
- **Another project's server:**
  - It is allowed. Automatic installs work only when it publishes valid signed statements (D5); otherwise the panel behaves as *Ask me first*.
  - BL-186's warning, "can install firmware that changes everything this panel does" (`BL-186-update-server.js:126`), gains one sentence: once automatic installs are on, this publisher's updates can install with nobody pressing anything.
  - The master's row says "Updates come from another publisher", without the domain. That keeps BL-186's approved rule that Software update doesn't name the server (`:35`, `:69`).

**Why:**
- The setting is the household's consent to losing protection for a few minutes.
- The installer alone controls the server and the channel (OTA-022). If the installer could also raise the mode, one installer code, stolen or not, could install another publisher's firmware with nobody present.
- BL-186 already varies one page by role (`components/services/panel_web/web/panel-pages.js` `updatesPage`).

**Rejected:**
- Installer only.
- An installer ceiling over the master's choice.
- Installer and master as equals (the first draft).
- Defaults other than *Ask me first* (D10).

### D3. When
**Recommendation:**
- **Window:** Morning 10 AM-12 PM, or Afternoon 1-3 PM (the default). Night is not offered.
  - A 03:00 install was rejected (README:377).
  - A household that sleeps disarmed would lose fire and CO detection while asleep.
  - A daytime window is also when people are home during the update.
- **Local time:** the window follows `auto_clock`'s daylight-saving rules (`components/core/automation/include/automation/auto_clock.h:1-9`). While the clock isn't set, there is no automatic install (`clock_not_set`, `components/services/platform_esp/src/net_wifi.c:735-740`).
- **Conditions (OTA-035).** All must hold:
  1. The gate passes, with P3's additions.
  2. No alarm memory (`alarm_engine.h:51`).
  3. No troubles:
     - no fault, restore, trouble or verification on a fire, CO, 24-hour or panic zone in the last 10 minutes;
     - no active trouble on such a zone;
     - no `ALARM_ZS_SUPERVISION` or `ALARM_ZS_TROUBLE_MEM` on any zone;
     - `sys_troubles_restored == 0` and `dev_troubles == 0`;
     - no partition with `trouble_unack` (`include/alarm/alarm_snapshot.h:22-31`, `alarm_engine.h:47`).

     Why: a restart forgets trouble memory and restarts every supervision clock. Neither is stored (`alarm_config.h:107-128`; `engine.c:221`, `:226-231`; `zones.c:530-552`).
  4. No `storage` or `panel_fault` trouble.
  5. **Power, where the board can tell.** The panel must not be running on battery, and the battery must not be low. An unknown reading never counts as healthy (PWR-010, `docs/features/17-Power-and-Supervision/README.md:55`). This board can't sense AC loss and has no battery yet (PWR-003, `:48`; `:128`). With no battery, the panel running means it is on mains. With a battery but AC unknown, the battery must be charging or full. Otherwise the waiting reason is "Can't tell whether mains power is on".
  6. No alarm report still on its way, such as a report held in its abort window (`alarm_engine.h:137`). The exact test is still to be designed.
  7. **Nobody active for 10 minutes:** no arming or disarming, no touch, no Panel Manager or remote-keypad request. Ordinary door, window and motion zones don't count: they fault all the time in a home that is lived in and disarmed (AUT-004, `docs/features/41-Automations/README.md:49`). Fire, CO, 24-hour and panic zones are covered by condition 3, and exit and entry time by condition 1. The 10 minutes are a proposal, not a standard.
  8. No automation due to arm or disarm within 15 minutes. "Nothing catches up after a restart" (AUT-005, `:50`).
  9. No other long job: a language pack, a Z-Wave device update, a restore, an SD card format.
  10. A valid signed statement and the day's fresh answer (D5).

  The conditions are checked at the chosen minute, through the notice, and again at OTA-004's last moment before the slot switch.
- **Requests never depend on the alarm state.**
  - On every day an install is pending, the panel fetches the fresh answer at a minute drawn at the start of the window, armed or not, and uses it for the whole window.
  - It never sends a request because the conditions became true.
  - After the self-test it goes back to its normal, jittered schedule.

  Why: the server keeps IP addresses in its proxy logs for 30 days, and its privacy promise is that nothing links checks to a place.
- **At most one automatic restart per 24 hours**, the Wi-Fi module's included.

**Rejected:**
- Installing right after a disarm: the gap then falls just when someone has come in.
- A free start time, as Hue has ([How-To Geek, "How to Update Your Philips Hue Firmware"](https://www.howtogeek.com/570593/how-to-update-your-philips-hue-firmware/)).
- A Night window.

### D4. Armed through the window, deadlines, and a person's Install
**Recommendation:**
- **Armed, or not quiet, through the window:** the panel tries the next day. It never installs outside the window, and there is no deadline.
  - Windows forces an install after at most 35 days of pausing ([Microsoft, "Pause updates in Windows"](https://support.microsoft.com/en-us/windows/deployment/updates-lifecycle/pause-updates-in-windows)).
  - Android forces one after 30 days ([Android Developers, "Manage system updates"](https://developer.android.com/work/dpc/system-updates)).
  - Both are rejected: an alarm never gives up protection on a timer.
- **An important fix that has waited 7 windows:** a notice to the master, repeated weekly while it waits. It names the reason, including "The update server can't be reached", "Held by its publisher" and "The release statement has expired".
- **One control before an automatic restart: *Not today*.**
  - It keeps the verified image and moves only the restart to the next window.
  - It is the same button as Stop during the window and the notice.
  - There is no *Skip this version*: a household that wants to choose each version sets *Ask me first*.
  - Home Assistant's Skip stays inside Home Assistant ([Home Assistant, "Update"](https://www.home-assistant.io/integrations/update/)).
- **A person's Install** answers BL-191 (Q3).
  - *Install now* is offered only when OTA-035's conditions hold.
  - Otherwise the question offers "Install between 1 and 3 PM", with "today" or "tomorrow" worked out when the question is shown. It is a one-time consent that waits for the window and its conditions.
  - Why: today an Install pressed while armed restarts within a second of the next disarm (README:314; `ui_update.c:329`), exactly the moment D3 rejects.

### D5. Trust
**Recommendation:**
- **Signed release statements.**
  - When a release is published, the release script on the maintainer's PC signs a statement with a second offline key: {project, version, channels, critical or leads-to, minimum version, image SHA-256, issued, expires}.
  - The check answer carries the statement, and the server still holds no private key.
  - The panel restarts by itself only when the statement verifies, its SHA-256 equals the downloaded image's, the panel's channel is in its list, and it hasn't expired.
  - The release script re-signs the statements of the current releases on a schedule (Q6). A withdrawn release gets no new statement. An expired statement means no automatic install, and the panel shows that as the waiting reason.
  - With no valid statement, the panel behaves as *Ask me first*.
  - Why: [The Update Framework Specification](https://theupdateframework.github.io/specification/latest/) names the attacks: arbitrary installation, indefinite freeze, mix-and-match, and the compromise of one key. A fresh check that asks the same unsigned source adds nothing against them.
- **Until statements exist:**
  - Never install by itself a SemVer pre-release while the channel is Stable. SemVer puts a pre-release below its own release only (README:112).
  - Never install by itself an image whose signed project name differs from the running one's.
- **Keys (Q5):**
  - A release-only key, kept offline in a hardware token. ESP-IDF documents signing with an external HSM (Espressif page above).
  - Development, test and bench-fault builds use a different key.
  - The statement key is a second key, held separately.
  - Existing panels move to the release key through one release that a person installs.
  - Write down how keys are rotated and what to do when one leaks.
- **Downloads:**
  - An automatic download comes only from an `https` URL on the update server's own host, and follows only `https` redirects. Any other URL waits for a person.
  - It is capped at the size the answer gives, and at the slot's size.
  - Why: the image URL is absolute and may be on any host (`components/core/alarm_proto/include/alarm_proto/proto_fwsrv.h:87`), and OTA-003 allows HTTP (README:28).
- **The ready image (`ota/ready`):**
  - It holds the version, the file's SHA-256, the ELF SHA-256 and a digest of the trust record.
  - Any change of server, key or channel throws it away, and a ready image never blocks that change.
  - Its signature is checked again, against the keys trusted now, at start and before any install, *Install now* included.
- **Backups:**
  - A backup never carries the accepted key.
  - A restore sets *Ask me first* unless the restored server and key equal the running ones.
  - A restored third-party server needs the installer's fingerprint step again.
  - Today a backup keeps `ota/src` (`components/core/backup/src/backup_policy.c:35`, `:63`; `docs/features/14-Configuration-and-Installer-Programming/README.md:268-273`). What BL-186's trust record does in a backup is not specified.

### D6. When things go wrong
**Recommendation:**
- **Never install by itself:**
  - a version that failed here;
  - a release marked manual;
  - a pre-release on Stable, or another project name;
  - an older or equal version (OTA-014).

  For "failed here", `ota/ready` stores the file's SHA-256 and the full ELF SHA-256, with no slot XOR. A rollback copies both into a list of the last four failed images. The panel compares the offered `image.sha256` with that list before downloading, and the ELF SHA-256 from the downloaded app description before scheduling. A person can still install such a version, with a warning: "failed its self-test here on Oct 9".
  - ESP-IDF's own example refuses the last invalid version ([ESP-IDF native_ota_example](https://raw.githubusercontent.com/espressif/esp-idf/master/examples/system/ota/native_ota_example/main/native_ota_example.c)).
  - The C6 path already has a try-once memory (`cp_ota.c:244-261`).
  - Rejected: a retry budget such as "two tries". A failed self-test says the image is wrong for this panel.
- **Stepping stones:** one window each, so the next release follows in the next day's window.
  - Hue Bridge Pro hubs broke after a long pause in updates ([hueblog, "Philips Hue Fixes Hue Bridge Pro Update Bug"](https://hueblog.com/2026/07/14/philips-hue-fixes-hue-bridge-pro-update-bug/)).
  - Releases should be tested from old versions, not only from the one before.
- **The self-test:**
  - OTA-046 checks the outputs.
  - OTA-031 holds the rollback during exit time, entry time and alarms.
- **For 7 days after an automatic install (OTA-047):**
  - There is no background download into the inactive slot, so the previous good image stays.
  - Three PANIC or WATCHDOG starts within 24 h in those days switch the boot slot back at the next quiet moment (as OTA-004 defines it), mark the version as failed here, log it and notify.
  - The cost: a release that comes out in that week waits unless a person installs it.
  - Faults can show late: a smart thermostat's firmware problem once took about two weeks to show.
  - The task watchdog isn't subscribed yet (`docs/features/17-Power-and-Supervision/README.md:117-121`).
- **Downloads** resume with HTTP Range. After three days of failures for the same version, the panel stops trying and says "Couldn't download".
- **Spread:**
  - The first check comes 3 min after boot plus a random 0-30 min.
  - The interval is 24 h ± a random 2 h.
  - Each OTA-015 step is drawn between half and all of its length.
  - A `Retry-After` from the server sets the shortest wait.
  - Sources: [AWS, "Exponential Backoff And Jitter"](https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/), and EN 303 645 5.9-3, EXAMPLE 1.
- **The Wi-Fi module (OTA-045).** BL-188 already adds the second check before the module's restart (`docs/features/07-UI-LVGL-Pro/mockup/screens/updates-run.js`). On top of that:
  - The module's update counts as that day's one automatic restart, so it goes to the next day's window, never straight after a P4 self-test.
  - It starts only when OTA-035 holds.
  - The gate is asked again after the image streams, before `esp_hosted_cp_ota_activate`, and again before `esp_restart`. Today it is asked once, before the stream (`cp_ota.c:210-218`, `:150-176`).
  - The module's image is sent by itself only when its SHA-256 is covered by a signature the P4 verifies (inside the signed P4 image, or in the statement), and only when its version is newer. Anything else needs a person (`c6 update`).
  - Measure the stream time on the bench. The update server's C6 images are planned for later.

### D7. What people see
**Recommendation.** The words follow BL-188's vocabulary: "ready to install" asks for Install; "Waiting" installs by itself (`docs/features/07-UI-LVGL-Pro/mockup/screens/updates-run.js`).
- **Software update:**
  - *Ask me first*: "Version 0.8.1 is ready to install".
  - Automatic or scheduled: "Waiting to install version 0.8.1", with the detail "Installs between 1 and 3 PM once everything is disarmed".
  - Waiting reasons, in the panel's words: alarm memory, "A fire alarm is being checked", "A sensor is still in alarm", a trouble, "Can't tell whether mains power is on", the clock isn't set, "The update server can't be reached", "Held by its publisher", "Installs in a few days: new versions reach panels a few at a time", failed here before, statement expired.
- **The mode row's line, and the Install question:** "Burglary, fire and CO detection are off for about N minutes while it restarts", with N measured (P1). EN 303 645 5.3-12 asks that people are told when an update disrupts the device.
- **Home banner:**
  - Nothing before the window.
  - In the window: "Restarts for an update between 1 and 3 PM".
  - For the last 2 minutes: "Restarts for an update at 1:22 PM".
  - Then BL-188's banners, in BL-188's order.

  The banners are steady and blue, and the words change only at a new step (UI-020; `docs/features/07-UI-LVGL-Pro/mockup/screens/updates-run.js`). Neither banner wakes a dark screen or brightens a dimmed one (`:155`). Arming, a touch or a Panel Manager request in those 2 minutes moves the restart later in the window, or to the next day. Nothing is ever a sheet over Home (`:6-8`).
- **About** shows "Update" while a downloaded version waits, and "Updating" from the window's restart notice. Today BL-188 shows "Updating" from Install (`:231`).
- **Push**, once per version:
  - *Ask me first*: "Version 0.8.1 is ready to install".
  - Automatic: "The panel restarts for version 0.8.1 between 1 and 3 PM if everything is disarmed. The alarm is off for about N minutes."
- **Notifications, by stage, with no new setting (Q8):**
  - To **Trouble** (on by default): rolled back; automatic installs switched off; an important fix waiting; update server changed; the panel restarted after a crash or a watchdog (`system_start` with PANIC or WATCHDOG).
  - To **System**: ready, installed and stopped.
  - This needs a stage-aware `notify_category_ev()` with a matching Activity rule, because notifications and Activity share one set of buckets (`components/core/notify/include/notify/notify_words.h:32-34`). There is a precedent for routing an event type to Trouble: `ALARM_EV_AUTOMATION_FAILED` (`notify_words.c:1155`).
  - Why no new category: each service stores its categories as one byte, so a new bit reads as off on every panel already set up (`components/services/net_notify/src/net_notify.c:189-190`, `:225`, `:241`). EN 303 645 5.12-1 asks for "minimal decisions by the user".
- **Activity:**
  - Automatic installs are logged with `ALARM_SRC_SYSTEM` and an appended flag or argument meaning "automatic", worded "Installed by itself". Not `ALARM_SRC_SCHEDULE`: it prints "By schedule" (`notify_words.c:288`; `components/core/alarm_core/src/strings.c:262`) and means automatic arming (`arming.c:133`, `:636`).
  - Log the settings changes, Not today and the stages. Don't log a record for every day it waits.
  - BL-188's Updated row gains "installed by itself at 1:22 PM" (`docs/features/07-UI-LVGL-Pro/mockup/screens/updates-run.js`).

### D8. Home Assistant and MQTT
**Recommendation:**
- **Show the plan as attributes** (`json_attributes_topic`): `auto_install`, `window`, `next_attempt` (an ISO time) and, from the 2-minute notice, `restart_at`.
  - `release_summary` stays the release's notes. Home Assistant defines it as a short excerpt of at most 255 characters ([Home Assistant Developer Docs, "Update entity"](https://developers.home-assistant.io/docs/core/entity/update/), re-read).
  - Today's discovery has none of these (`discovery.c:241-254`). They are additions, which the contract allows (`components/core/AGENTS.md`).
- **Before every restart**, a person's included: publish a retained "offline" on the availability topic. Home Assistant then shows the panel unavailable, and an arm sent during the gap fails visibly. Today the broker would queue nothing (clean session), and HA hears of the restart only from the last will, up to 45 s later ([OASIS, "MQTT Version 3.1.1"](http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html), §3.1.2.4 and §3.1.2.10; not re-read). Don't switch to a persistent session: it would deliver old disarms after long outages.
- **The RoboAlarms integration (ha-addon):**
  - The link's `status.update` gains an optional `auto` object (`docs/features/23-Home-Assistant-Integration/protocol.md:36-43`, an API 1 extension). Both repositories change together.
  - `auto_update` is set true when the panel will install the offered version by itself. In HA that means "you cannot skip updates" (same HA page).
- **Home Assistant's Install (Q4):**
  - It stays: BL-188 approved it (`docs/features/07-UI-LVGL-Pro/mockup/screens/updates-run.js`; OTA-004, README:29).
  - It becomes an "Install between 1 and 3 PM": held to OTA-035, OTA-036 and OTA-039 and inside the window, because it carries no user (`net_mqtt.c:1026-1027`).
  - Home Assistant never changes the mode or the window, and never presses Not today or Stop. "Firmware installation remains authorized on the panel" (`protocol.md:42-43`).

### D9. The update server
**Recommendation:**
- **Staged rollout:** a percentage per release, stepped by the server's administrators and audited.
  - The panel works out its own turn: SHA-256(salt ‖ version) mod 100, with a random salt kept in NVS, never sent and never backed up. That is how Ubuntu phases updates ([Ubuntu, "Phased updates"](https://ubuntu.com/project/docs/how-ubuntu-is-made/concepts/phased-updates/)); Sparkle does the same ([Sparkle, "Publishing an update"](https://sparkle-project.org/documentation/publishing/)).
  - The rollout gates only automatic installs and the "ready" push. A person can still install by hand, as with Apple's phased releases ([Apple, "Release a version update in phases"](https://developer.apple.com/help/app-store-connect/update-your-app/release-a-version-update-in-phases/)).
  - Pacing: at least 2-3 days per step, and a week before 100 %.
- **Pause and resume**, for automatic installs only. Withdrawing stays final.
- **A "never by itself" flag per release.**
- **Signed statements:** uploaded with the release, stored and served exactly as uploaded. The server checks them against a configured public key and never signs anything.
- **The check answer** gains optional fields: `rollout {percent, paused}`, `manual` and `statement`.
- **The check request** gains `want: "critical"`, and the critical-release rules are amended to match.
- **Install results (Q7):** recommended *not now*. If ever added:
  - they only alert the administrators, and never pause anything automatically;
  - at most one report per /24 (IPv4) or /48 (IPv6) network a day;
  - only for versions the server offered that day.

  Anyone can forge a report, and results conflict with the promise of "nothing sent just for statistics".

**Rejected:**
- A dice roll per request ([Mozilla Balrog, "Database Model"](https://mozilla-balrog.readthedocs.io/en/latest/database.html)): it isn't sticky, so at 10 % about half the fleet gets the release within a week (1 - 0.9^7, our calculation).
- A bucket number sent by the panel: it would link one day's checks to another's.
- A deadline field.
- An automatic pause driven by reports anyone can forge.

### D10. The default, and what the law and standards expect
**Recommendation:** *Ask me first* by default. Write the safety reasoning, the timeframe for security fixes and the departures below into ADR-051. The ADR reverses README:377 and changes ADR-023's reasoning.

**Why:**
- **The standard written for these products names this risk.** The draft standard for smart-home security products ([Draft ETSI EN 304 632 V1.0.0 (2026-07)](https://www.etsi.org/deliver/etsi_en/304600_304699/304632/01.00.00_20/en_304632v010000ev.pdf), re-read) turns automatic updates on by default only where they do "not create an unacceptable risk to essential function availability or safety" (5.4.2). 5.5.3 NOTE 1 names "delaying egress during a fire alarm" and "preventing timely arming/disarming/panic activation". 5.4.3 wants update notifications on by default, and 5.3.2 an initial security update before or during first use, "if not blocked by the entity that puts" the device into first use.
- **The CRA's update rule.** CRA Annex I Part I (2)(c) asks for automatic security updates "installed within an appropriate timeframe", "enabled as a default setting", with an opt-out, notification and "the option to temporarily postpone them" ([Springlex, "CRA Regulation - Annex I"](https://www.springlex.eu/en/packages/cra/cra-regulation/annex-1/), re-read). This design meets all of it except the default and the timeframe: there is no deadline, and the ADR says why.
- **The CRA's separate-releases rule.** Part II (2) asks for security updates provided separately from functionality updates "where technically feasible". The `critical` flag alone doesn't meet that; D2's patch releases do.
- **ETSI EN 303 645 V3.1.3** (re-read):
  - It recommends turning automatic updates on "after user's consent" (5.3-4B), and notifying people of a disruption (5.3-12).
  - In 5.3 these provisions are mandatory: 5.3-2, -3, -7, -8 ("Security updates shall be timely"), -10, -13 and -16 (Table B.1).
  - The text after 5.3-12 expects safety-relevant devices "not to turn completely off" during an update. The restart departs from that, and the ADR records it.
  - Clause 5.3 calls separating security updates from feature updates "desirable".
- **The CRA does not bind this project today**, but could later:
  - Non-monetised free software isn't a commercial activity ([Springlex, "Recital 18"](https://www.springlex.eu/en/packages/cra/cra-regulation/recital-18/)).
  - The draft guidance warns that a paid service built on it can count ([Commission draft guidance on the CRA](https://www.law.co.il/media/computer-law/cra_draft_guidance.pdf), para 51).
  - Alarm systems are Class I ([Springlex, "Annex III"](https://www.springlex.eu/en/packages/cra/cra-regulation/annex-3/)).
  - Recital 56 allows a person approving as the final step ([Springlex, "Recital 56"](https://www.springlex.eu/en/packages/cra/cra-regulation/recital-56/)).
- **Alarm-industry rules lean towards a person approving.** EN 50131-1 puts changes to the operating software at access level 4 ([NSI, "NSI-Technical-Bulletin-0054"](https://members.nsi.org.uk/?p=33864)). NFPA 72 asks for a test after a software change ([Security Sales, "NFPA 72 Changes: Remote Access & UL Usage"](https://www.securitysales.com/fire-intrusion/nfpa-72-changes-remote-access-ul-usage/)).
- **Other law.** UK PSTI asks only for a published support period ([UK SI 2023/1007 Schedule 1](https://www.legislation.gov.uk/uksi/2023/1007/schedule/1/made)). The EU Product Liability Directive leaves out non-commercial free software ([Streamlex, "PLD Art. 2"](https://streamlex.eu/articles/pld-en-art-2/)).

**Rejected:**
- *Important fixes* on by default: revisit once P1-P8, the rollout and the statements exist (Q10).
- *All updates* on by default: our restart is the boot plus a 60 s settle.

## 4. Draft requirements

### Panel (docs/features/13; changes to approved requirements are marked)
| ID | Requirement |
|---|---|
| OTA-004 (changed) | Restart only when safe: today's gate, plus no fire verification running and no alarm zone still faulted (P3). A person asks for the install, or the panel installs by itself under OTA-032..045. |
| OTA-007 (changed) | The Wi-Fi module's image is sent by itself only under OTA-045. "Differs" becomes "newer". |
| OTA-024 (changed) | Drop "Nothing installs by itself". |
| OTA-031 (changed, approved) | A failed self-test rolls back at once when the engine or sensor-input heartbeat is missing. When only the screen, touch, log or a new output check failed, it waits while any partition is in exit or entry time, or a bell or alarm sounder is on: at most the longest exit or entry time plus `bell_timeout_s`, and never more than 5 min. Crashes stay with the bootloader. |
| OTA-032 | **Install mode:** *Ask me first* (the default; Kconfig `PANEL_OTA_AUTO_DEFAULT`), *Important fixes by themselves*, or *All updates by themselves*. The system master sets it. The installer can only set it back to *Ask me first*. Refused while the master or installer code is the factory one. Back to *Ask me first* when the server or key changes, when the channel changes to anything but Stable, or when a restore brings a different server or key. Dev channel, a development address or `http://`: no automatic install. Logged with who. An update never raises it. |
| OTA-033 | **Background download:** under every mode, only over `https` on the server's own host, capped at the answer's size and the slot size. Resumes with HTTP Range. Not on a development address, not during probation or another run, and not during OTA-047's hold. `ota/ready` holds the version, the file SHA-256, the ELF SHA-256 and the trust digest. The image survives a restart, is checked again against the current keys at start and before any install, and is thrown away on a change of server, key or channel. |
| OTA-034 | **Window:** Morning 10-12 or Afternoon 13-15 (the default), in local time with `auto_clock`'s rules. Nothing while the clock isn't set. |
| OTA-035 | **Conditions** for an automatic or scheduled install: D3's list 1-10. Checked at the chosen minute, through the notice, and at OTA-004's last moment. |
| OTA-036 | **Trust and the fresh answer:** a restart by itself only with a valid, unexpired signed statement whose SHA-256 equals the image's and whose channels include the panel's, and with the day's fresh answer still offering it (not withdrawn, not paused, the panel's turn reached, not manual). Server unreachable: no restart that day. Requests never depend on the alarm state. |
| OTA-037 | At most one automatic restart per 24 h, the Wi-Fi module's included. The next check follows the normal schedule. |
| OTA-038 | **Notice:** the window banner, and the 2-minute banner before the restart. Neither wakes or brightens the screen. Activity in those minutes moves the restart. Before every restart, a retained "offline" on MQTT. `restart_at` from the start of the notice. |
| OTA-039 | **Never by itself:** a version that failed here (last four; file SHA-256 and full ELF SHA-256), a release marked manual, a pre-release on Stable, another project name, an older or equal version. A person can still install it, with a warning. |
| OTA-040 | Armed or not quiet through the window: the next day. Never outside the window, and no deadline. An important fix that has waited 7 windows: a notice every week, with the reason. |
| OTA-041 | *Not today* keeps the image. Stop during an automatic window or notice means *Not today*. No Skip. *Install now* only while OTA-035 holds; otherwise "Install between …" (BL-191, Q3). Home Assistant's Install is the latter (Q4). |
| OTA-042 | **Spread:** 3 min + random 0-30 min after boot; 24 h ± 2 h; OTA-015 steps drawn between half and the whole; `Retry-After` honoured. |
| OTA-043 | **Rollout turn:** SHA-256(salt ‖ version) mod 100, with a per-panel salt that is never sent and never backed up. |
| OTA-044 | **Telling people:** `ALARM_SRC_SYSTEM` plus an "automatic" flag. Notification routing by stage (D7). MQTT attributes, and the link's `update.auto`. |
| OTA-045 | **The Wi-Fi module:** counts as the day's restart; next window; OTA-035; the gate asked after the stream and before the restart; a signature-covered SHA-256; newer only. |
| OTA-046 | **Self-test outputs** (automatic and scheduled installs): before the restart, record which of these work: the audio or sounder task heartbeat, MQTT connected, the last notification delivered, each radio source up. After the restart, each of those must report within 180 s, or the panel rolls back (subject to OTA-031). New health bits are appended (`panel_ota.h:142-143`). |
| OTA-047 | **After confirmation:** a 7-day hold on background downloads. Three PANIC or WATCHDOG starts within 24 h switch back to the previous image at the next quiet moment, mark the version as failed here, log it and notify. |
| OTA-048 | **Backups** never carry the accepted key or `ota/ready`. A restore sets *Ask me first* unless the server and key are unchanged. A restored third-party server needs the fingerprint step again. |
| OTA-049 | (Only if Q7 is yes) the first check after an install carries `last_update {version, result}`. |

### Engine (docs/features/01, /17)
| ID | Requirement |
|---|---|
| CORE-PWR-008 | During any settle, a fault on a fire, CO, 24-hour or panic zone from a radio sensor is latched and alarms when the settle ends. After a planned software start, the settle covers wired loops only, for a measured time. Host test: a 24-hour panic fault and restore inside the settle alarms after a software start. |
| CORE-PROG-013 | The power-up recovery opens only after `ALARM_START_POWER_ON` (and optionally BROWNOUT and EXTERNAL). Host test: a software start gives no recovery. |
| (PWR-007 routing) | `system_start` with a PANIC or WATCHDOG reason goes to Trouble. |

### Update server (the changes it needs)
- A rollout percentage per published release (default 100), changed by its administrators and audited.
- Pause and resume a release's rollout. It stops automatic installs only; withdrawing stays final.
- A "never install by itself" flag per release.
- Signed release statements, made offline and uploaded, then served exactly as uploaded. The server checks them against a configured public key and never signs. Re-signed statements are uploaded by the release script.
- Check answer: optional `rollout {percent, paused}`, `manual` and `statement`.
- Check request: optional `want: "critical"`. The answer then offers the oldest reachable critical release, or the stepping stone toward it. The critical-release rules are amended.
- (Only if Q7 is yes) `last_update` counts per day: alerts to the administrators only, at most one per /24 or /48 a day, only for versions offered that day. The "nothing sent just for statistics" promise and the privacy notice are reworded.

## 5. UI changes that need mockups first (UI-044, WEB-026)

**Approved screens this changes** (each must be marked "changes an approved screen"): settings-update-install-question, settings-update-downloading, settings-update-waiting, settings-update-stop-question, settings-update-self-test (the hold), home-update-banner and home-update-armed (banner words and when they show), about-update-running, settings-update-wifi-module, settings-update (the ready row) and installer-update-server-warning (one sentence). In the Panel Manager: updates/interrupted and updates/offered-armed (`components/services/panel_web/web/panel-pages.js` `updCard`).

**Touchscreen:**
1. Software update: the *Install updates* and *When* rows and their pick sheets, with the line "about N minutes". The master's view and the installer's read-only view. The Dev row, dimmed. "Updates come from another publisher". "Automatic installs are off: the update source changed".
2. The ready and waiting rows, with every waiting reason.
3. The Install question: *Install now* only when the conditions hold, otherwise "Install between 1 and 3 PM" (today or tomorrow).
4. *Not today*, and Stop meaning *Not today*.
5. Home: the window banner and the 2-minute banner, placed in BL-188's banner order.
6. Updated "installed by itself", and "failed its self-test here" on a version that is offered.
7. Notifications settings and the Activity filter, if Q8 is no.
8. Activity words for the new stages and the settings changes.
9. (Q9) the setup step.

**Panel Manager:** the *Automatic installs* card (the master's and the installer's views), the status wording, "Install between …" and *Not today*.

**Update server administration:** the rollout steps, pause and resume, the manual flag and the statement upload. Rollback counts only if Q7 is yes.

## 6. Questions for the maintainers

| # | Question | Recommended answer |
|---|---|---|
| Q1 | Reverse the 2026-09-28 decision "no - a person asks" (README:377), with *Ask me first* as the default and the system master able to opt in? | Yes, recorded as ADR-051, and only after P1-P8. |
| Q2 | Download every offered version in the background on every panel, replacing BL-188's Install-then-download? | Yes. It costs no protection, and Install becomes a restart only. |
| Q3 | BL-191: does a person's Install also wait for OTA-035, the 10 quiet minutes included, becoming "Install between …" when the conditions don't hold? | Yes. Today it restarts within a second of the next disarm. |
| Q4 | Home Assistant's Install over MQTT: keep it (approved in BL-188), held to the window and all the conditions, or remove it? | Keep it, held to the window and the conditions. |
| Q5 | Keys: a release-only key in a hardware token, never shared with other machines or a vault, and a second offline key for the statements? Development builds keep today's key, wherever it is shared. | Yes. One stolen key should not be enough to reach every panel that has opted in. |
| Q6 | The release script re-signs the current releases' statements every 7 days, with a 14-day expiry, on the maintainer's PC. Stale statements stop automatic installs. Acceptable as a chore? | Yes, as a scheduled task. A longer expiry delays how fast a withdrawal reaches the panels. |
| Q7 | Anonymous install results from panels, against the update server's "nothing sent just for statistics"? | Not now. Staged rollout, alpha and beta, and an admin watching are enough at this size. |
| Q8 | Notifications: route by stage (Trouble or System), or add a *Software updates* category with an NVS migration and an Activity filter? | Route by stage. The panel's own "Update" tag is the notice that is on by default. |
| Q9 | First-run setup: check for a security update and offer it (EN 303 645 5.3-4B, EN 304 632 5.3.2)? | Yes, offer *Install now* during setup, when nothing is protected yet. Keep the mode at *Ask me first*. |
| Q10 | Revisit *Important fixes* as the default once P1-P8, the rollout and the statements exist, and the gap is measured? | Yes, then. |

## 7. Documents to bring in step

- **docs/features/13:**
  - a decision entry replacing `:377`;
  - OTA-004, OTA-007, OTA-024 (`:49`) and OTA-031;
  - the sentence "nothing installs by itself" (`:18-20`);
  - the NVS keys (`:294`).
- **Kconfig help:** `components/services/panel_ota/Kconfig:24`.
- **ADR-051 in docs/features/00**, with ADR-023's reasoning changed.
- **docs/features/14:** the update rows of the access table, the "Software updates are the household's too" choice, and the backup table.
- **docs/features/01 and /17:** CORE-PWR-008, CORE-PROG-013 and PWR-002.
- **docs/features/26:** the routing.
- **docs/features/23 `protocol.md`**, and the ha-addon repository.
- **The update server's documents:** rollout, pause, statements, the check answer and request, and the release-script section.
- **BL-190's row:** rewrite it to this design, and answer BL-191 in it.

## 8. What I could not verify

- **On a panel:**
  - how long the restart and the settle really take, and when MQTT reconnects;
  - whether `esp_restart()` cuts power to wired 4-wire smoke detectors;
  - whether Zigbee and Z-Wave raise "unreachable" again quickly after a restart;
  - whether the boot screen respects night brightness;
  - whether `esp_hosted_cp_ota_activate` resets the C6 at once.
- **Design details still open:**
  - the exact test for "no report still on its way";
  - switching the boot slot back to the previous image after confirmation (OTA-047) is untested on this board;
  - how BL-186 treats the trust record in a backup (it is not specified);
  - `panel_ota_server_set_dev` is still a stub (`components/services/panel_ota/src/panel_ota_stubs.c:36`).
- **Web sources re-read for this version:** CRA Annex I, EN 303 645 V3.1.3 (text pulled out of the PDF, not rendered), Draft EN 304 632 (the same way), the Espressif Secure Boot v2 page (it doesn't say whether key revocation works without eFuses), the TUF specification, and Home Assistant's update-entity page.
- **Everything else on the web** comes from the research brief and was not re-read: Tesla, UniFi and Synology were snippets only. The MQTT 3.1.1 sections come from a critique.
- **Proposals, not standards:** the legal reading is not legal advice. The 10- and 15-minute quiet times, the 7-day hold, the rollout pacing and the 14-day statement expiry are this design's proposals.

## 9. Critique not applied

- *Rules and UX, "delete question 8":* not applied. The safety and security critiques show that Home Assistant's Install bypasses every new condition, so it stays as Q4, reworded.
- *Security and alarm safety, "remove the MQTT install, or require confirmation at the panel":* not decided here. It is held to the window and the conditions instead (Q4), because BL-188 approved Home Assistant's Install.
- *Security, "show the server's domain and key fingerprint to the master":* replaced by "Updates come from another publisher". The domain would break BL-186's approved rule (`BL-186-update-server.js:35`, `:69`), and a fingerprint means nothing to a homeowner.
- *Security, "any channel change resets the mode":* narrowed to changes to anything but Stable (rules and UX). Moving to Stable lowers the risk.
- *Rules and UX, "the installer sees the mode read-only":* the installer may still lower it to *Ask me first* (security), which is the safe direction.
- *Alarm safety, "condition 6: no zone event of any kind":* narrowed to fire, CO, 24-hour and panic zones and to troubles. Ordinary door and motion activity would block every install in a home that is lived in (rules and UX).
- *Alarm safety, "latch fire, CO, 24-hour and panic faults during any settle":* narrowed to radio sensors, plus wired loops only after a software start. Latching wired loops after a power-on would bring back the false alarms the settle exists to stop.
- *Security, "the release key is never sent to other machines" as a precondition:* made Q5: today one key signs every build.
- *Rules and UX, the "fewest settings that work" rule:* no such rule was found in the repository. The recommendation stands on EN 303 645 5.12-1 instead.

## Sources

**Web, re-read for this version:**
- CRA Regulation - Annex I (Springlex): https://www.springlex.eu/en/packages/cra/cra-regulation/annex-1/
- ETSI EN 303 645 V3.1.3: https://www.etsi.org/deliver/etsi_en/303600_303699/303645/03.01.03_60/en_303645v030103p.pdf
- Draft ETSI EN 304 632 V1.0.0 (2026-07): https://www.etsi.org/deliver/etsi_en/304600_304699/304632/01.00.00_20/en_304632v010000ev.pdf
- Secure Boot v2 - ESP32-P4 (Espressif): https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/security/secure-boot-v2.html
- The Update Framework Specification: https://theupdateframework.github.io/specification/latest/
- Update entity | Home Assistant Developer Docs: https://developers.home-assistant.io/docs/core/entity/update/

**Web, from the research brief or a critique (not re-read):**
- MQTT Update - Home Assistant: https://www.home-assistant.io/integrations/update.mqtt/
- Update - Home Assistant: https://www.home-assistant.io/integrations/update/
- MQTT Version 3.1.1 (OASIS): http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html
- CRA Recital 18, Recital 56, Annex III (Springlex): https://www.springlex.eu/en/packages/cra/cra-regulation/recital-18/, https://www.springlex.eu/en/packages/cra/cra-regulation/recital-56/, https://www.springlex.eu/en/packages/cra/cra-regulation/annex-3/
- Commission guidance on the CRA (draft): https://www.law.co.il/media/computer-law/cra_draft_guidance.pdf
- PLD Art. 2 (Streamlex): https://streamlex.eu/articles/pld-en-art-2/
- UK SI 2023/1007 Schedule 1: https://www.legislation.gov.uk/uksi/2023/1007/schedule/1/made
- NSI-Technical-Bulletin-0054: https://members.nsi.org.uk/?p=33864
- NFPA 72 Changes: Remote Access & UL Usage: https://www.securitysales.com/fire-intrusion/nfpa-72-changes-remote-access-ul-usage/
- Updating Firmware on a Shelly Device: https://shellyusa.atlassian.net/wiki/spaces/SU/pages/559153188/Updating+Firmware+on+a+Shelly+Device
- Managed Updates via HTTP Request (ESPHome): https://esphome.io/components/update/http_request/
- Pause updates in Windows: https://support.microsoft.com/en-us/windows/deployment/updates-lifecycle/pause-updates-in-windows
- Manage system updates (Android): https://developer.android.com/work/dpc/system-updates
- How to Update Your Philips Hue Firmware: https://www.howtogeek.com/570593/how-to-update-your-philips-hue-firmware/
- Phased updates (Ubuntu): https://ubuntu.com/project/docs/how-ubuntu-is-made/concepts/phased-updates/
- Publishing an update (Sparkle): https://sparkle-project.org/documentation/publishing/
- Release a version update in phases (Apple): https://developer.apple.com/help/app-store-connect/update-your-app/release-a-version-update-in-phases/
- Balrog Database Model: https://mozilla-balrog.readthedocs.io/en/latest/database.html
- Exponential Backoff And Jitter (AWS): https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/
- Philips Hue Fixes Hue Bridge Pro Update Bug: https://hueblog.com/2026/07/14/philips-hue-fixes-hue-bridge-pro-update-bug/
- ESP-IDF native_ota_example: https://raw.githubusercontent.com/espressif/esp-idf/master/examples/system/ota/native_ota_example/main/native_ota_example.c

**Code and documents:** cited inline by file and line, as of 2026-10-08.
