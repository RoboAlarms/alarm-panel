# 07 · User interface with LVGL Pro

**Status:** Implemented v1 - Home, the code pads, the lists, Settings, installer mode, the
first-boot setup, Integrations, Software update (with its progress, Stop and the update server, BL-186 and BL-188) and zone sources work, checked by the PC
preview and in daily use on a panel. Left: panel checks of the later changes
(BL-024, BL-029), a cheaper glow (BL-059), SD saver pictures
(BL-060) and the last of the approved redesign (BL-131: Activity as the whole log, waiting on a
decision; the rest of it and the four appearances, BL-132, are built) ·
**Milestone:** M3, M4, M5, M6, M11 ·
**Depends on:** 04, 05, 06, 08, 09, 10, 13, 14, 22

**Code:** `components/ui/alarm_ui` (the logic in `src/`, the LVGL Pro projects in
`layouts/<WxH>/alarm_ui`), `components/services/ui_port_esp` (the panel's backend) ·
**Tests:** `host/ui_preview/main.c` (the scripted preview, `pwsh tools/ui_preview.ps1`, run in
CI, with `UI-NNN` checks), `host/ui_preview/smoke_minimal.c` (the UI with every optional page
left out); CI also runs `tools/lvpro_gen.py --check`, `tools/check_ui_contract.py`,
`tools/embed_image.py --check` and `tools/embed_logo.py --check`

## Goal
A modern touch UI for the 1024x600 screen that keeps a classic keypad panel's behaviour: designed in the
**LVGL Pro Editor** (XML, version-controlled), exported to plain C, and backed by logic that runs
the same on the panel, on a remote keypad (M7) and on a PC.

The top bar includes battery status beside Wi-Fi (PWR-011, docs/features/17). Missing batteries and unavailable sensing
use a steady red crossed-out battery; valid measurements show the level and percentage.
Low battery remains amber. The native STC source validates cell voltage before displaying its percentage; an empty
connector stays red despite the helper reporting 100%. Charging uses the lightning icon. The monitor still distinguishes unknown from absent internally.

| No battery (simulated) | Battery percentage (simulated) |
|---|---|
| ![Crossed-out battery](screens/82_battery_absent.png) | ![Battery at 82 percent](screens/83_battery_full.png) |

The preview also covers [unknown sensing](screens/81_battery_unknown.png),
[low battery](screens/84_battery_low.png), and
[unavailable percentage](screens/85_battery_unavailable.png). The 100% case is checked with
the factory-code banner, chime and trouble count visible so Settings stays within the screen.

![Charging with a valid helper estimate](screens/86_battery_charging.png)

![Home, ready to arm](screens/01_home_ready.png)

## Design language
- **Dark and calm, colour means state.** Every state has one tone: green disarmed and ready,
  amber not ready / troubles / alarm memory, blue Stay, violet Night, rose Away, orange entry
  time, red alarms. The tone colours the state ring, the rail logo and the check key of the pad.
  During an alarm the whole screen and the state card turn red and the ring pulses.
- **One glance from 3 m** (UI-003): the state ring (236 px) with its icon or the seconds left,
  the state in 42 px type, one line saying what it means, one line of detail.
- **Two panes on Home**: the state on the left, what you can do on the right (arm tiles; from
  the exit time on the Disarm pad itself, with no card to tap first (UI-036); clear alarm
  memory, end walk test).
- **iPhone-style code entry**: a dot per digit (codes are 4-10 digits, so the length is open),
  a header laid out like the screen saver's state card - the icon of the tile or card it stands
  for on a tinted disk spanning two lines, the action as the title (in its colour) over the
  hint, left-aligned beside it, the pair centred - with the X in the card's top-right corner;
  the check key (in the action's colour) submits. Wrong codes shake the dots and turn the hint red.
- **Arming owns Home; everything else asks over the screen** (UI-025). The same pad opens as a
  panel on a dimmed backdrop, over the view you were on, and adds a line of plain
  words under the title: "Turn chime on - a beep at the panel each time a door or window opens, while the
  system is disarmed". Nothing about arming is in sight while you do it.
- Inter (SIL OFL 1.1) for text, Lucide (ISC) line icons, 64 px or larger touch targets
  (keys are 120x64).

The rail logo takes the state's tone (UI-037):

![Rail logo in the existing state colours](screens/rail-logo-colors.png)

| | |
|---|---|
| ![Not ready](screens/02_home_not_ready.png) | ![Arm Away code pad](screens/03_pad_arm_away.png) |
| ![Exit time](screens/04_exit_delay.png) | ![Armed Away](screens/05_armed_away.png) |
| ![Entry time](screens/06_entry_delay.png) | ![Wrong code](screens/07_pad_wrong_code.png) |
| ![Burglary alarm](screens/08_alarm_burglary.png) | ![Alarm memory](screens/09_alarm_memory.png) |
| ![Zones](screens/10_zones.png) | ![Activity](screens/11_activity.png) |
| ![Troubles](screens/12_troubles.png) | ![Armed Night](screens/13_armed_night.png) |
| ![Fire alarm](screens/14_fire_alarm.png) | ![Emergency, holding Police](screens/15_emergency_hold.png) |
| ![Change your code](screens/16_change_code.png) | ![Screen saver](screens/17_screen_saver.png) |
| ![Screen saver while armed](screens/68_screen_saver_armed.png) | ![Armed: Settings and Chime locked](screens/69_armed_locked.png) |
| ![Arming Instant: quiet exit time](screens/70_exit_instant.png) | ![Chime asks over the Zones view](screens/78_chime_panel.png) |
| ![A bypass asks the same way](screens/79_bypass_panel.png) | ![Chime by one touch when the installer allows it](screens/80_chime_one_touch.png) |

### Settings, installer mode, first-boot setup (M4) and integrations (M5)

| | |
|---|---|
| ![Display and sound](screens/18_settings_display.png) | ![Users: the master code first](screens/19_settings_users_code.png) |
| ![Users](screens/20_settings_users.png) | ![On-screen keyboard](screens/21_keyboard.png) |
| ![What this code can do](screens/22_user_authority.png) | ![A user](screens/23_user_page.png) |
| ![Wi-Fi (master or installer code)](screens/24_settings_wifi.png) | ![Wi-Fi password](screens/25_wifi_password.png) |
| ![The cog: how the panel gets its address](screens/26_network_address.png) | ![Checking the connection](screens/27_network_check_running.png) |
| ![Every check passed](screens/28_network_check_passed.png) | ![From the router, or a fixed address](screens/29_network_address_mode.png) |
| ![A fixed address](screens/30_network_static.png) | ![The wrong router: the check says so](screens/31_network_check_failed.png) |
| ![No address from the router: the rest was never reached](screens/32_network_check_no_address.png) | ![Date and time](screens/33_settings_time.png) |
| ![Integrations: Home Assistant and MQTT, each with its state](screens/34_integrations.png) | ![Home Assistant: the integration, off](screens/35_ha_integration.png) |
| ![Switched on: ready to pair](screens/36_ha_ready.png) | ![Pairing open, waiting for Home Assistant](screens/37_ha_pairing_wait.png) |
| ![Both screens show the code](screens/38_ha_pairing_code.png) | ![Paired](screens/39_ha_paired.png) |
| ![MQTT, not set up](screens/40_mqtt_setup.png) | ![Broker address](screens/41_mqtt_address.png) |
| ![MQTT, connected](screens/42_mqtt_connected.png) | ![Security choice](screens/43_mqtt_security.png) |
| ![Not encrypted: the warning](screens/44_mqtt_not_encrypted.png) | ![Activity, the whole event log](screens/45_event_log.png) |
| ![Installer, locked](screens/46_installer_locked.png) | ![Installer menu](screens/47_installer_menu.png) |
| ![System options](screens/48_installer_options.png) | ![Zones](screens/49_installer_zones.png) |
| ![A zone](screens/50_zone_edit.png) | ![Zone type](screens/51_zone_type.png) |
| ![Factory reset: hold](screens/52_factory_reset.png) | ![About](screens/53_about.png) |
| ![About: an update is available](screens/54_about_update_available.png) | ![About: update waits until disarmed](screens/55_about_update_waiting.png) |
| ![About: an update was rolled back](screens/56_about_rolled_back.png) |  |
| ![Setup: welcome](screens/57_setup_welcome.png) | ![Setup: Wi-Fi](screens/58_setup_wifi.png) |
| ![Setup: time zone](screens/59_setup_time_zone.png) | ![Setup: new installer code](screens/60_setup_new_code.png) |
| ![Setup: codes](screens/61_setup_codes.png) | ![Setup: done](screens/62_setup_done.png) |
| ![Home after the setup](screens/63_home_after_setup.png) | ![Lost installer code: power-up recovery](screens/64_installer_recovery.png) |
| ![Recovery: confirm](screens/65_recovery_confirm.png) | ![Recovery: set a new installer code](screens/66_recovery_new_code.png) |
| ![Wi-Fi asks for a master or installer code](screens/67_wifi_locked.png) |  |

### Zone sources (M11)

| | |
|---|---|
| ![Where is the sensor?](screens/71_zone_source_pick.png) | ![Pick the terminal: the one just opened is marked](screens/72_zone_terminals.png) |
| ![A new wired zone following its contact](screens/73_zone_wired.png) | ![A sensor that stopped answering: Check](screens/77_zone_unreachable.png) |
| ![The shared Home Assistant catalog in the picker (M13)](screens/74_zone_ha_catalog.png) | ![Narrowed with the search box](screens/75_zone_ha_search.png) |
| ![A zone fed by a Home Assistant entity](screens/76_zone_ha_bound.png) |  |

These are real renders: `pwsh tools/ui_preview.ps1` runs the real alarm engine, the event log
(on a RAM flash), the exported UI and its logic on a headless LVGL display, taps through a
scripted evening, Settings, installer mode, the setup and a power-up recovery with a virtual
touchscreen and writes these PNGs (CI does the same and uploads them as an artifact). The
Home Assistant connection in the preview is simulated (a broker named `*.invalid` doesn't
answer, the password `wrong` is refused); the zone hub is the real one with the simulator and
the wired source on fake J7 terminals. The Installer > Z-Wave pages (`ui_zwave.c`, builds with Z-Wave) render
only with `pwsh tools/ui_preview.ps1 -ZWave` (shots 132-150, not in `screens/`).

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| UI-001 | UI source of truth is the LVGL Pro project of each screen size, `components/ui/alarm_ui/layouts/<WxH>/alarm_ui/` (XML; `1024x600` for this panel, `480x320` for the 4", docs/features/19); its exported C is committed next to the XML so builds don't need the editor; every layout keeps the contract the UI code relies on (`components/ui/alarm_ui/layouts/README.md`) | CI (`lvpro_gen.py --check` per layout, `tools/check_ui_contract.py`) |
| UI-002 | Screens read state through subjects; `ui_app.c` computes the subjects from status snapshots and never touches the engine directly (only through `ui_backend_t`). Dynamic lists (zones, events, troubles) are filled from C | code review |
| UI-003 | Home shows status (ready / not ready / level / countdown / alarm) readable from 3 m: large text, colour **plus** icon and words | preview, device |
| UI-004 | Arming: Away / Stay / Night tiles; options No entry delay (the Panel Manager's Overview's words, docs/features/38 BL-114; the pad title stays "Arm Away", "Arm Stay", "Arm Night" and the chip says the option - not "Instant" or "Maximum", which mean nothing to someone who just tapped Away; the state card keeps "Arming Away" / "Armed Away" and says "No entry delay" in its note), Silent exit (Away only: Stay and Night exit time is quiet anyway), Bypass open zones; code pad with a dot per digit (4-10), the check key submits. Every mode keeps the exit time ([01](../01-Core-Alarm-Engine/README.md) "Arming modes") | preview (`70`) |
| UI-014 | "Factory codes in use" banner while `default_codes` is non-zero; tapping it opens "Change your code" (current code, new code twice) | preview |
| UI-005 | Exit/entry countdown: seconds in the ring, the ring empties; entry time opens the pad by itself | preview |
| UI-006 | Alarm: red screen and state card, pulsing ring, zones in alarm, pad open; fire and CO say so and offer Silence | preview |
| UI-007 | Zones list (state per device class, including open and closed while bypassed; bypass/unbypass when disarmed, filters; an empty Open, Bypassed or Trouble filter shows the green check and its phrase - "No open zones", "No bypassed zones", "No zone troubles" - at once after a filter tap, and after a zone's change the list and the check swap at most once a second, both ways, BL-177), troubles list (acknowledge, paged: UI-030), Activity: the whole event log (filters, paging, LOG-008's private step; BL-131 finding 7) | preview; `rf_bypassed_closed`, `rf_bypassed_open`, `rf_unbypassed_open` |
| UI-008 | Panic buttons (police, fire, medical) need a 2 s hold with a progress ring; what the hold does is said in plain, local words (UI-029) | preview |
| UI-009 | Screen saver after 60 s idle in calm states: a photo (the forest cabin at night, from the picture pack; the firmware image does not carry it, and the saver stays plain dark without it; SD-card pictures later), shaded on the left; the clock and date top left; a state card bottom left with the state's colour, icon, words, what it means and troubles; backlight 25 % by default (Screen and sound > Screen saver brightness, 0-50 %); any touch or state change wakes it at the normal brightness; then dark, and night hours: UI-043. Every change of state (arming, the end of the exit time, disarming, entry time...) also restarts the idle time, so the new state stays in view for the full saver time: after a 120 s exit time and a 30 s saver setting, the saver shows 30 s after the panel is armed. Not during exit or entry time. Static: nothing moves or blinks | preview (`17`, `68`; the walk-through checks the saver stays off right after the exit time), panel |
| UI-010 | Dark theme, and Light, High contrast and High contrast, light (the layout's `themes.xml`, BL-132), switched at run time in one step; touch targets 64 px or larger. Picked in Screen and sound with no code, like the brightness (UI-015), and in the Panel Manager's Display and sound: **Appearance** is Dark (the default, so an updated panel keeps its look), Light or Automatic - dark during the night hours (UI-043) and light by day, switching once at each end (a switch due while an alarm sounds or entry or exit time runs waits until it ends); with the night hours off it says so and stays light, and before the panel knows the time it stays light - and **High contrast** is a switch that works with any of them (Dark + High contrast is white on black, Light + High contrast black on white). A change applies at once; the screen saver keeps the dark colours. Stored in `ui/appearance` and `ui/contrast`; a value this firmware doesn't know reads as Dark | review; preview (`theme_*`, `settings_display_light`, `settings_display_contrast`, `appearance_pick`), `tools/tests/test_lvpro_gen_themes.py`, `test_ui_backend` (`ui_light_theme`), `panel_pages.test.cjs` |
| UI-011 | Keypad role: pairing screen, "No system communication" screen | M7 |
| UI-012 | Installer screens: the menu (opened from About) with Updates (the update server and the channel, UI-021), system options (delays, arming including remote arming without a code, codes, power, recovery), zones (add, edit, remove), system name, installer and master codes, run the setup again (leavable, UI-033), factory reset (hold to confirm). Keypads (M7), firmware (M6) and backup come with their milestones | preview |
| UI-013 | UI regression screenshots rendered headless in CI | CI (`ui-preview` job) |
| UI-015 | Settings: sections on the left, one line each in the rail's style (an icon and the name; a tag only when something needs you, About's "Update" while an update waits), Screen and sound, Users, Wi-Fi and network, Time and language, Integrations, Automations (when the build has them, docs/features/41), Walk test, About (BL-131; Installer is the last row of About, under System details, BL-189; the event log is Activity's since finding 7). Pages on the right with Back. Who opens what follows docs/features/14 "Who can change what": Screen and sound (brightness, volume, key clicks, screen saver, screen off and night hours (UI-043); the chime row follows UI-025), About (Software update inside it asks for the installer's or the system master's code, UI-021), and Time and language's clock rows (what the clock says, the 12/24-hour Clock) need no code; Users a master code; Wi-Fi and network, the rest of Time and language (time zone, language, region, time server; docs/features/37) a master or installer code (as do Activity's private events, docs/features/05 LOG-008); Integrations the system master or installer code; Installer, at the bottom of About, the installer code (or the power-up recovery). A code opens every section it allows until Settings closes | preview (`24`, `67`), code review |
| UI-016 | On-screen keyboard for names, network names and passwords (lower/upper case, numbers, symbols; Lucide glyphs on the special keys); a password's field ends in an eye that shows what was typed and hides it again, and every sheet opens with it hidden (BL-131); choice lists and confirmations over the screen | preview |
| UI-017 | First-boot setup on its own screen: welcome, Wi-Fi (scan, password, hidden network), time zone, the date and time (only where a clock module lost the time and there is no internet: docs/features/35 RTC-015), change the factory codes (optional), system name, done; every step skippable; an alarm takes the screen over and the setup continues after it | preview |
| UI-033 | **The setup run again can be left.** On a panel that is already set up ("Run the setup again" in the installer menu), every step's footer offers "Leave setup" beside Back: it goes back to Home at once, and what the steps already did (Wi-Fi, time zone, codes, name) is saved as it happens, so there is nothing to undo and nothing to confirm. On a first boot the setup has nothing to go back to and the button isn't shown: it ends at "You're all set". The setup screenshots below are a re-run, so they show the button | preview (the walk-through runs the setup again, checks the way out is there on the welcome and the Wi-Fi step, and that it lands on Home) |
| UI-018 | General screens never mention a particular integration (Home Assistant): Settings > Integrations lists them, each with its own page (M5). Exceptions (docs/features/22; docs/features/24's devices, planned): the source picker when adding a zone or device, and the "Source" row of a zone or device that uses it | review |
| UI-020 | **Nothing flashes** (photosensitivity safety: a panel hangs on a wall and is looked at in the dark by whoever walks past). No blinking, strobing or flashing backgrounds, text or icons, no backlight flicker. Alerts use steady colour, icons, words and sound. The only animations are short press feedback and, while an alarm sounds, the state ring's slow glow: a smooth fade, 1.3 s per cycle, far below WCAG's 3 flashes a second, kept by choice (decided 2026-09-29). Text sheets' cursor stays steady (`ui_forms_steady_cursor()`, [B003](../../bugs/B003-text-cursor-blinked.md)). Don't speed it up or make it a hard on/off. Anything new that moves needs a review against this rule and WCAG 2.3.1 (at most 3 flashes a second, and we aim for none). The display pipeline itself is guarded too ([06](../06-CrowPanel-P4-Platform/README.md) P4-012) | review, `display stress` on the panel |
| UI-021 | Settings > About > Software update ([13](../13-OTA-Updates/README.md)): it opens for the installer or the system master (their code, asked once per visit to Settings); everyone else sees only "Update", "Updating" or "Rolled back" on About's row. Status (up to date, ready to install, withdrawn or critical, downloading with a level bar, size and percent, checking the download, waiting for disarm and why, restarting, self-test, updated, rolled back, stopped, failed; the Wi-Fi module's update and a developer file have their own words), the steps of a run, Install (its question says the update can be stopped), Stop the update (a question; until the restart) and Check again. While a release update runs a steady blue banner in the status bar says the step (never a sheet over Home, arming, the pad or Panic; it gives way to an alarm). The update server and the channel are not here: they are the installer's, in Installer > Updates (OTA-020..OTA-022), held while an update runs | preview (`about_update_available`, `upd_downloading`, `upd_stop_question`, `home_update_banner`, `software_update_locked`, `fwsrv_menu`) |
| UI-022 | **Locked while armed** (Tools and Settings aren't offered while armed or in alarm). From the exit time on, or while an alarm sounds, on any partition: the Settings gear and Chime are dimmed, and they, "Acknowledge" on Troubles (code + OFF, which would disarm) and the factory-code banner (hidden) say "... once the system is disarmed" instead of opening. Arming from elsewhere (Home Assistant, a keyfob) closes Settings, forgets the code typed there, closes sheets and every pad but Disarm, and pauses the setup. Home with Disarm, Zones and Troubles (to look at), Activity and the panic buttons stay. The engine enforces the same for users and chime ([01](../01-Core-Alarm-Engine/README.md) CORE-USR-023, CORE-CHIME-004) | preview (`69`; the walk-through checks the gear and Chime stay shut and that remote arming closes Settings and forgets the code) |
| UI-023 | Zone sources in the zone editor ([22](../22-Zone-Sources/README.md)): "Add a zone" asks "Where is the sensor?" (the sources usable now, real sensors first, the simulator last) and makes the zone once a sensor is picked; a wired contact is picked from the terminals, shown live (Open / Closed, the one just opened or closed marked in amber, terminals in use greyed with their zone); the zone page adds Source, Contact and Response time for wired contacts, "Right now", and what the source can promise in words, warnings in amber; the zone list says where each sensor is. The source kinds come in one order (wired, Home Assistant, Z-Wave, Zigbee, 433 MHz), and a new zone made from a picked sensor starts with the type suggested for its kind ([22](../22-Zone-Sources/README.md)), not Perimeter for all. A 433 MHz zone's tamper and low-battery inputs are codes of its own sensor, picked per code or by hand (docs/features/25 RF-021). Live words change at most once a second (UI-020) | preview (`49`, `50`, `71`-`73`, `77`; the walk-through checks each step) |
| UI-024 | **Every list scrolls from anywhere on it.** A finger put down on plain content - a read-only line, an event, a setting's name, an empty gap - drags the list under it, the same as one put down on a button. Dragging from a button scrolls without pressing it; a tap still presses it. Scrolling makes no key click. In LVGL a drag starts only on an object the touch can land on, so every scrolling box is `clickable` as well as `scrollable`, and carries `user_1` to say it is a surface to scroll rather than a control | preview (the walk-through drags Activity, About, System options, the choices picker and the setup, and fails if a list doesn't move, if the touch lands on something else, or if scrolling clicks) |
| UI-025 | **What isn't arming asks where you are.** Chime, bypass, acknowledging troubles and "change your code" would switch to Home and put the code pad beside the arming state card, which reads as "you are arming". They open a code panel over the view instead, dimmed behind it, with the action's icon, the action as the title ("Turn chime on") and a line of plain words saying what it does; the X changes nothing and 90 idle seconds close it and forget the digits. Arming, disarming, clearing alarm memory and ending a walk test keep Home: the state card belongs to them. Chime also has a row in Settings > Screen and sound ([14](../14-Configuration-and-Installer-Programming/README.md)), Chime, a round switch in the status bar beside the gear, shows a bell when it is on and a crossed-out bell when it is off, and with the installer option off ([01](../01-Core-Alarm-Engine/README.md) CORE-CHIME-005) one touch switches it | preview (`78`-`80`; the walk-through checks the chime and bypass panels open over Zones and leave Home alone, that the X changes nothing, that a code switches the chime, and that one touch does once the option is off) |
| UI-019 | **MQTT is an integration of its own**, listed beside Home Assistant in Settings > Integrations, not a page underneath it (the two are separate things). The list shows a row per integration with what it is doing right now, refreshed once a second (UI-020). The MQTT page: live status and why a connection fails, connect switch, broker address, port, user name, password (never shown), security with a warning for "Not encrypted", announce the panel (discovery), arm-and-disarm switch, whether arming needs a code, topic prefix and panel id; every change applies at once. Its words assume no particular system - any broker, Node-RED, your own server or scripts - and while the local Home Assistant integration is paired it says why no discovery messages go to the broker ([23](../23-Home-Assistant-Integration/README.md) HAI-010) | preview (`34`, `40`-`44`; the walk-through reaches MQTT from the list and checks the paired note) |
| UI-027 | **Home Assistant pairing on the panel** ([23](../23-Home-Assistant-Integration/README.md) HAI-003): Settings > Integrations > Home Assistant leads with the integration - a status row in plain words (off, waiting for Wi-Fi, ready to pair, waiting for Home Assistant, check the code, paired, connected), the enable switch, Pair (a 2-minute window), then both screens show the same 6-digit code with "allow" and "reject" actions and a warning to compare them; paired shows the client and a hold-to-Unpair. The page rebuilds only when the state changes (polled once a second, UI-020). The page is about the integration alone: MQTT is a separate entry in the list (UI-019) | preview (`35`-`39`; the walk-through pairs with the pretend Home Assistant, checks the code 588460 - golden vector 1 - and the paired client) |
| UI-028 | **A toast never repeats what the screen shows.** Arming, disarming, chime, bypass, acknowledging troubles, clearing memory and every "Saved" whose row visibly updates show nothing extra: the state card, the zone card, the rail's bell or the rebuilt row is the confirmation. Toasts remain for refusals (with the reason), for outcomes nothing on screen shows (a changed code, the silent panic, the deferred factory erase), and for next steps ("Keep holding...", "Siren silenced. Enter your code to reset") | review; preview (the walk-through passes with no toast after arming or chime) |
| UI-029 | **The Emergency screen says what the panel did, never that help is coming.** Holding a button for 2 seconds sets the alarm off *on this panel*: the screen says exactly that, and a second line says whether anything is set up to carry it further and whether that thing is reachable right now ("Nothing is connected to pass this on", "A connected service is set up but offline now", "A connected service is online and is sent the alarm"), each ending "The panel can't call emergency services". Four things stay apart: the alarm being on here, reporting being configured, reporting being reachable, and a monitoring service answering - which this panel has no way to know, so it never claims it. A command the engine accepted and a broker that took a message are neither of the last two. Letting go early says "Keep holding for 2 seconds to set the alarm off" and changes nothing. The silent panic stays discreet and still truthful: "Silent alarm on", or "Silent alarm on · nothing sent out" when nothing can carry it. The second line is written at most once a second (UI-020) and names no integration (UI-018) | preview (`15`; the walk-through checks the wording with nothing connected, with a service set up but offline, and with one connected, that "call for help" is on no screen, and that a short press sets nothing off) |
| UI-030 | **The trouble list shows every fault and counts them all.** Object allocation stays bounded - 48 rows - but that is a page, not a limit: the list walks the system's troubles, then the keypads', then the zones' by number, **active faults before restored memories** - the Panel Manager's walk (`alarm_snapshot_troubles`, BL-140), with the sensor radio's trouble a row per radio in trouble - and a pager under the list ("49-61 of 61", with a key each way that wraps round) reaches the rest. It appears only when there is more than one page. The count above the list is of everything the snapshot holds, on this page or not. The status bar's pill is the way to the list and counts everything it holds, restored troubles not acknowledged yet included; with nothing in the list the pill isn't there (BL-131). Leaving the view and coming back starts at the active faults again. This matters more since a single source outage can raise a supervision trouble on many zones at once ([22](../22-Zone-Sources/README.md)) | preview (the walk-through floods 60 zones with a fault, checks the pager appears, that the note agrees with the status bar's count, that the last zone's fault can be reached, and that paging comes back to the first) |
| UI-031 | **Nothing covers the keyboard.** One rule decides where a toast sits, and it is worked out whenever either side changes - not only when the toast appears. The text sheet's card is at the bottom, so the toast moves to the top and every key, Space and Done included, stays visible and reachable; a choice list, a confirmation or the code pad over the screen fills the middle, so a toast from before it opened goes (it has been read by now). Nothing is left hovering: the toast is hidden rather than moved out of sight, and it is not clickable, so it can take no touch meant for a key | preview (`21`, `22`; the walk-through checks the toast and the keyboard don't overlap, that a touch anywhere on the keyboard reaches the keyboard, and that a choice list has no toast over it) |
| UI-032 | **An unfinished code entry is forgotten.** Half a code is a credential: after 90 idle seconds the digits, the half-typed new code and any error go, on **every** pad - Home, Settings, the first-boot setup and the panel over the screen (UI-025) - and the pad closes onto a safe view (Settings rebuilds the page behind it, usually "locked"). With the Home pad gone the screen saver can start, which an abandoned prompt used to prevent. Entry time, a sounding alarm and the Disarm pad that is Home's right side from the exit time on (UI-036) are the exception, and only about the pad itself: disarming must stay at hand and the screen must go on showing the alarm, so the pad stays exactly where it is and only what was typed is dropped (the screen saver still starts over an untouched Disarm pad while armed). Settings still forgets the code that opened it after five idle minutes | preview (the walk-through abandons a code on Home, in Settings and during an alarm, and checks each one) |
| UI-026 | **The network's details stay behind a cog.** Settings > Wi-Fi keeps the one thing most people ever need - which network, and is it connected - and a cog on the connection row opens "Network" for the rest: how the panel gets its address (from the router, or fixed with address, network mask and router), the name servers, and "Check the connection". The cog stays there while the panel is offline, so a fixed address typed wrong can be undone from the screen. A fixed address is applied only once it is complete; until then the page says what is missing and the panel keeps asking the router. The check's result is a list of steps, each with a tick, a cross or an amber warning and a line saying what it found or where it went wrong; it runs by itself after an address change. Its rows are built once and only their words change afterwards, so nothing reflows or flickers while it walks (UI-020, UI-023). The connected service's step is named for what it is, not for the integration (UI-018). [09](../09-WiFi-Time-and-Provisioning/README.md) NET-011, NET-012 | preview (`26`-`31`; the walk-through switches to a fixed address, breaks the router address, checks that the check reports it, and puts it back) |
| UI-034 | **Integrations is grouped, and says so.** The list is two headed groups - "Smart home" (Home Assistant, MQTT) and "Notifications" (ntfy, Pushover) - because it no longer offers one kind of thing, and the page's note says so ("Smart-home systems, and alerts to your phone"; the Settings list names sections only, BL-131). Each row keeps its service's name and carries what it is doing right now on the line underneath, rewritten in place so the list never reflows (UI-020, UI-023). A whole group disappears when the platform doesn't offer it, the way Home Assistant already does. The general screens still name no service (UI-018): a notification page is reached only from here | preview (`87`; the walk-through fails if either heading is missing) |
| UI-036 | **No card before the code** (a card to tap first adds a touch that isn't needed). From the exit time on - armed Away, Stay or Night, entry time, alarms - the right side of Home is the Disarm pad ("Cancel arming" during the exit time, "Disarm" after), not a "Cancel" or "Disarm" card that opens it. It has no X, since there is nothing behind it to go back to; it carries the unlocked-padlock icon the card had. Arming from elsewhere puts it there without taking anyone off the view they are reading (Zones stays Zones); a pad over the screen that closes during those states hands back to it. Installer mode keeps its card (a tap that leaves installer mode, not a code), and alarm memory and walk test keep theirs | preview (`04`, `05`, `70`; the walk-through checks the pad is up with no X and an icon during the exit time and once armed, disarms with no tap before the code, and that the header doesn't move when the dots replace the hint) |
| UI-035 | About is one page: the version and Panel ID on top, Software update, the SD card, then what System details held (This panel, Network, Set up, Hardware, read-only) and Installer last (BL-189). Show SD state/capacity with protected formatting and firmware/flash space with honest reserved allocations; remove the affiliation disclaimer and retain library attribution | preview (`about`, `about_end`, `about_update_available`), docs/features/29 |
| UI-037 | The RoboAlarms brand mark replaces only the shield above Home. It uses the same steady state colour as the Home status display, including ready, not ready, Stay, Night, Away, entry and alarm, with the existing anti-flicker dwell preserved (the Home card's colour changes at most every 500 ms unless the new state is an alarm or entry delay). The mark is a 44x44 embedded A8 alpha mask (1,936 bytes) tinted from `subject_tone_color`; `tools/embed_logo.py` reproduces the C resource and CI checks the pair. Keep the Home button and all other state icons unchanged | preview checks source presence and tint on every rail-visible capture; generator check; visual state crops |
| UI-038 | **Browser sign-in is set on the panel** (and in the Panel Manager by whoever manages the user). The page of every user who can have one - the installer, a system or partition master, a standard user; not arm-only, guest or duress - has a "Panel Manager" section: *Allow browser sign-in* (or *Change the browser password* and *Turn off browser sign-in* once on), with the user name it signs in with; the installer's is also in the Installer menu under Remote access, its user name the installer's name ("Installer" from the factory). About shows the Panel ID. The password is typed twice in the password sheet (never shown), at least 12 characters; one set for somebody else here is temporary (in the Panel Manager the person setting it may let them keep it; docs/features/31 "Sign-in") | preview (`53`, `116`-`119`; the walk refuses a short password, turns a master's sign-in on and off, gives a new standard user one and checks an arm-only code has none, and the installer sets the master's, temporary) |
| UI-039 | **The remote keypad is visible at the panel.** While someone uses the panel from the network (the Panel Manager's remote keypad, an enrolled workstation), the status bar shows a steady red banner, "Remote: <who> · Tap to pause", ahead of the installer and factory-code banners, and the screen saver stays off. A tap on it at the panel (never from the remote itself) pauses the remote and asks: *Allow* or *Turn off remote access* (no code: turning it off only protects; the installer turns it back on at the panel). The pause belongs to the panel, not the session: a remote that leaves and comes back is still paused ("Remote keypad paused · Tap for choices") until the person at the panel answers, and the question stays open for them. Nothing blinks (UI-020) | preview (`120`, `121`: the banner, the pause, Allow, turning remote access off, the banner going with the session) |
| UI-040 | **A keyboard at the remote end types on the panel.** Text goes into the text sheet on screen (its length and characters apply), digits into the code pad; Backspace, Enter (Done / the check key) and Escape (Cancel / close) work on both. Nothing else takes typing | preview (`120`: a name retyped and saved, digits and backspace in a code pad, escape closing it) |
| UI-041 | **Everything the Panel Manager sets, the installer sets at the panel:** every system option (cancel window, alarm limit, cross-zone time, fire verification, smoke reset, exit restarts, recent closing, settling time, walk test, trouble beeps, panic keys, relaxed limits), partitions 1-4 (in use, name, delays), keypads and devices, a zone's partition, the user a button acts as, supervision, cross-zone partner, alarm limit and "never shut down", its sensor's reading (mode, threshold, hysteresis, states), reverse, response time, unreachable time, tamper and low-battery inputs, removing the sensor but keeping the zone; a user's partitions; the screen saver's times and a dark saver, the screen going dark and the night hours (UI-043). Certificate authorities (pasted PEM) and developer tools stay browser-only | preview (`122`-`127`) |
| UI-042 | **Setting the date and time by hand** (docs/features/35): the first row of Time and language says what the clock says ("Saturday, September 19 · 10:41 PM", its minute kept current) and what set it last ("Kept right by the internet", "Kept while the panel was off", "Set by hand"; amber "Not set" while nothing has). Where the panel can keep a time (a battery-backed clock module answers) the row opens a sheet, "Set the date and time": a month calendar (its arrows change the month, a tap on the month opens month and year rollers) beside hour, minute and, on a 12-hour clock, AM/PM rollers that snap without sliding, a line saying what the clock will say, and Set; the setup's clock step has the same calendar and rollers (BL-131 finding 3). **The hardware is never named** (decided: an end user would not know about the module); its state is for the console and the Flasher | preview (`clock_set`, `clock_set_month`, `setup_clock`) |
| UI-043 | **The screen goes dark, and night hours** (for a keypad in a bedroom). Settings > Screen and sound > Screen off: Never (the default), 30 s to 1 hour; it counts from the last touch like the screen saver, and the first of the two wins, so a screen-off time shorter than the saver's goes straight to dark and a longer one dims first, then goes dark. Dark is the saver's screen with the backlight off, so the touch that wakes it presses nothing. **Night hours** (off by default; 22:00 to 07:00 in quarter hours, wrapping past midnight; the start is in, the end is out): the screen in use is at the night brightness (10-100 %, default 20 %), the saver never brighter than it, and the screen goes dark after the night's own time (default 1 minute, Never possible). They begin and end by the clock alone, and never while the panel doesn't know the time (the page says so). What wakes it is UI-009's (a touch, a change of state, entry time, an alarm, a remote session); troubles don't (their beeps call). While an alarm sounds the screen is at the day brightness, at night too. One step per change, never back and forth (UI-020): a brightness slider shows its level for 3 s, then the level of the moment. The Panel Manager sets the same (UI-041). The rules are `components/core/ui_backend/include/ui_backend/ui_light.h` | host tests (`test_ui_backend.c`), preview (`147`, `148`; the walk checks the dim, dark and wake times by day and at night, straight to dark, an alarm at night, no time no night, and night hours beginning and ending untouched), simulator (the picture fades with the backlight), panel |
| UI-044 | **A change a person sees is drawn and approved first.** The touchscreen's screens as they are today live in the HTML mockup ([`mockup/`](mockup/README.md)), built from the layout's tokens, fonts and Lucide icons. A new or changed screen, row, control, flow or wording starts there as a proposal tied to its backlog row, and is built in the XML and C only once the maintainers have approved it (decided 2026-10-01: agents changing the UI without input caused usability problems). When it lands, the mockup's screens are updated to match, so the mockup stays the panel as it is | CI (`ui_mockup.py check`: names, icons, tokens, preview pictures, proposals), `ui_mockup.py compare` |
| UI-045 | **A zone's spoken name is picked from the voice's words.** Installer > Zones > the zone > Spoken name finds, orders, previews and approves vocabulary words, says which parts of the zone's name it can't speak, and says when the voice is off, missing or the name needs review ([08 voice-first-build.md](../08-Audio-and-Annunciation/voice-first-build.md)) | UI preview `walk_voice` (`host/ui_preview/main.c`, the `voice_*` pictures, mockup `zone-spoken-*`, `spoken-*`, `settings-announcements-*`): the row's states, Cancel and its lease, eight words and over 10 s, Find a word's pages, Use zone name's unmatched part, the voice language, approval with the save, closure when installer mode ends |

### Notifications (M15)

Two services that put an alert on a phone with no server of the owner's own, grouped under
Integrations beside the smart-home pair (UI-034, docs/features/26-28). ntfy arrives with an
unguessable topic already generated and the hosted service already chosen, so switching it on is
the only step, and the QR code saves typing the topic into a phone; Pushover takes the owner's
own two credentials and defaults alarms to the priority that repeats until somebody answers.

| | |
|---|---|
| ![Integrations, grouped](screens/87_integrations_notifications.png) | ![ntfy as it arrives](screens/88_ntfy_page.png) |
| ![Hosted or your own server](screens/89_ntfy_server.png) | ![Switched on](screens/90_ntfy_on.png) |
| ![Turning alarms off says what that means](screens/91_ntfy_alarms_off.png) | ![The QR code, asked for and warned about](screens/92_ntfy_qr.png) |
| ![A server that stopped answering](screens/93_ntfy_unreachable.png) | ![Pushover](screens/94_pushover_page.png) |
| ![A user key that isn't one](screens/95_pushover_bad_key.png) | ![Both credentials stored, neither readable](screens/96_pushover_on.png) |


## Toolchain
- **LVGL Pro Editor 2.0.4** (free for open-source projects; optional - `lvpro_gen.py` does the
  same export) opens
  `components/ui/alarm_ui/layouts/1024x600/alarm_ui/` for live preview and drag-and-drop design mode. **Ctrl+B** exports C code, which
  overwrites the `*_gen.c/.h` files and `fonts/*_data.c`. Commit the result.
- **`tools/lvpro_gen.py`** exports the same files without the editor: the editor's own exporter
  and the `lvglpro` CLI need a paid licence, so the repo can't rely on them in CI. It supports the
  subset of the XML this project uses, rejects unknown attributes and references, and writes code
  in the editor's layout (create functions per component and screen, `alarm_ui_init_gen()`,
  subjects, `file_list_gen.cmake`), so either tool can take over at any time. Two additions the
  editor doesn't make (BL-132): the layout's `themes.xml` becomes a palette table that the
  `COLOR_*` constants read, and each style's initialisation registers it
  (`alarm_ui_style_track`, hand-written in `alarm_ui.c`) so a theme switch reaches it; after an
  editor export, run the script again (CI's `--check` fails until then).
  ```
  python tools/lvpro_gen.py components/ui/alarm_ui/layouts/1024x600/alarm_ui            # after editing XML
  python tools/lvpro_gen.py components/ui/alarm_ui/layouts/1024x600/alarm_ui --fonts    # after changing fonts (needs lv_font_conv)
  python tools/lvpro_gen.py components/ui/alarm_ui/layouts/1024x600/alarm_ui --check    # CI: generated files match the XML
  ```
  Fonts use `lv_font_conv` 1.5.3 with the editor's options (`--bpp 4 --format lvgl --range ...
  --symbols ...`, and `--no-compress` for the text fonts). The icon fonts (`icons_*`) and the big
  display fonts (`font_headline`, `font_digits`, docs/features/39 F2) come out RLE-compressed
  (lossless, `font_compressed()` in the script), so every build needs
  `LV_USE_FONT_COMPRESSED` (`components/ui/alarm_ui/layouts/README.md`). Point `LV_FONT_CONV`
  at it, e.g. `node <tools>/node_modules/lv_font_conv/lv_font_conv.js` (see the script's header).
- **`tools/ui_preview.ps1`** (Windows) or `cmake -S host/ui_preview ...` (Linux/macOS) builds and
  runs the PC preview.
- LVGL is pinned to **9.5.0** in `project.xml` and `components/ui/alarm_ui/idf_component.yml`.

## Project layout

```
components/ui/alarm_ui/layouts/1024x600/alarm_ui/                      LVGL Pro project (target crowpanel_7in, 1024x600)
  project.xml globals.xml         design tokens, fonts, global styles, the subjects
  alarm_ui.c / .h                 custom part: the XML's event callbacks -> one action handler
  alarm_ui_gen.c / .h             generated
  components/                     in folders base, common, forms, home, lists, nav, pad, panic,
                                  settings, sheets: box, icon_chip, badge, chip, segment,
                                  view_header, nav_item,
                                  nav_toggle (a rail entry whose icon is a state: Chime),
                                  hero, arm_tile, action_tile, pad_key, code_pad, zone_card,
                                  event_row, trouble_row, panic_button, settings_item, list_row,
                                  row_value, row_switch, row_stepper, row_slider, row_button,
                                  section_title, hold_button, text_sheet, pick_sheet,
                                  confirm_dialog, modal_pad (the code pad over the screen,
                                  UI-025) (+ *_gen.c/.h)
  screens/                        screen_main (rail, status bar, 6 views, sheets, toast),
                                  screen_saver, screen_wizard (first-boot setup)
  fonts/                          Inter + Lucide TTF sources, licences, generated *_data.c
components/ui/alarm_ui/layouts/480x320/alarm_ui/   the 4" board's project (docs/features/19, on hold)
components/ui/alarm_ui/              ESP-IDF component around the project
  include/ui_app.h                ui_app_start() and the options; includes the contract,
                                  ui_backend_t (components/core/ui_backend)
  src/ui_internal.h               state and helpers shared by the modules below
  src/ui_app.c                    view model, code pad (Home, Settings, setup, over the screen),
                                  chime, lists, panic, saver
  src/ui_forms.c                  settings rows, keyboard, choice lists, confirmations
  src/ui_settings.c               Settings: sections, who opens what, page stack, display,
                                  users, event log, about
  src/ui_installer.c              installer mode: options, zones, name, codes, reset
  src/ui_sources.c                zone sources: "Where is the sensor?", the terminal picker, the
                                  zone page's source rows and trust facts (docs/features/22)
  src/ui_net.c                    Wi-Fi (status, scan, password, hidden network, failures), the
                                  cog's Network page (fixed address, name servers, "Check the
                                  connection", UI-026) and date and time
  src/ui_integrations.c           Integrations: the Home Assistant link and MQTT
  src/ui_notify.c                 ntfy (with its QR code) and Pushover (docs/features/26-28)
  src/ui_update.c                 About > Software update, its run view and the status-bar banner (docs/features/13)
  src/ui_storage.c                About > SD card (docs/features/29)
  src/ui_zwave.c                  Installer > Z-Wave, only in builds with Z-Wave (docs/features/36)
  src/ui_saver.c                  the screen saver's picture: JPEG decoded once to RGB565 (TJPGD)
  src/ui_status.c                 the status screen of a board whose RAM can't hold the full UI
                                  (state, troubles, clock, a ring under the finger; docs/features/19)
  images/                         JPEGs embedded with tools/embed_image.py (see images/README.md)
  src/ui_wizard.c                 first-boot setup
  src/ui_tz.c                     time zones (POSIX rules) offered in setup and Settings
  src/ui_text.c                   icons, tones, the zone card and the words only this screen
                                  uses; the event sentences come from notify's notify_words.c
components/services/ui_port_esp/  backend on platform_esp (alarm_service, panel_log, net_wifi,
                                  net_check, panel_prefs, sounder, panel_system, zone_service),
                                  net_mqtt and panel_ota; later also ha_link, net_notify,
                                  panel_clock, panel_storage and zwave_service (it was port/ in
                                  components/ui/alarm_ui until docs/features/33 step 5)
host/ui_preview/                  PC preview: real engine + UI + scripted screenshots
```

A box that scrolls needs `clickable="true" user_1="true"` next to its `scrollable="true"`
(UI-024): LVGL starts a drag only on an object the touch can land on, and `user_1` tells
`ui_app.c` it is a surface to scroll rather than a control, so scrolling makes no key click.

## Screens

```mermaid
flowchart LR
  HOME[Home: state + arm/disarm] -- tap a level --> PAD[Code pad]
  PAD --> EXIT[Exit countdown + pad] --> ARMED[Armed + pad]
  ARMED -- door opens --> ENTRY[Entry countdown + pad]
  ENTRY -- time runs out --> ALARM[Alarm: red + pad]
  ALARM -- code --> MEMORY[Alarm memory -> clear]
  HOME --> ZONES[Zones + bypass] & ACT[Activity: the event log; private events need a master/installer code] & TRBL[Troubles + acknowledge] & PANIC[Emergency: hold 2 s]
  HOME -- 60 s idle --> SAVER[Screen saver] -- touch / state change --> HOME
  HOME -- gear --> SET[Settings]
  SET --> DISP[Screen and sound] & USERS[Users: master code] & NET[Wi-Fi and network: master/installer code] & TIME[Time and language: the zone and language need a master/installer code] & INTEG[Integrations] & WALK[Walk test] & ABOUT[About]
  ABOUT --> INST[Installer: installer code or power-up recovery] --> OPT[Options] & ZN[Zones] & NAME[Name] & PARTS[Partitions] & DEV[Keypads and devices] & CODES[Codes] & AGAIN[Run the setup again] & RESET[Factory reset] & UPD[Updates: server and channel]
  FIRST((first boot)) --> SETUP[Setup: Wi-Fi, time zone, codes, name] --> HOME
```

Settings keep a stack of pages on the right (Back returns, and leaving an installer page saves
it). Leaving the Installer section or Settings ends installer mode; Settings go back to Home
after 5 idle minutes (other views after 90 seconds), which also forgets any code typed there.

The navigation rail holds places (BL-131): Security (Home in the code and in this document),
Zones, Activity, and Panic at its foot; Smart Home joins it, third, with its screen
(docs/features/24 HCTL-003). The status bar (date, or the remote keypad, installer-mode or
factory-code banner; the trouble pill, Wi-Fi, battery, clock, Chime, settings) stays on screen.
The trouble count is said once, as an amber pill in the status bar that opens Troubles
(UI-030). Alarms and entry time bring Home forward by themselves; other views fall back to Home
after 90 s idle. While armed or in alarm, Chime and the settings gear are dimmed and only say
why (UI-022). Chime is a round switch beside the gear, not a place: it switches the chime where
you are (UI-025) and carries its state in its icon (bell, crossed-out bell), never a selected
style.

### The mockup
[`mockup/`](mockup/README.md) draws these screens in HTML from the layout's own tokens, fonts and
icons, beside the PC preview's pictures of the same states (`python tools/ui_mockup.py build
--shots`). A change to what a person sees or does on the touchscreen starts there as a proposal,
and is built only once the maintainers have approved it (UI-044).

## View model (subjects in `globals.xml`)

| Subject | Type | Meaning |
|---|---|---|
| `subject_view` | int | 0 Home, 1 Zones, 2 Activity, 3 Troubles, 4 Emergency, 5 Settings |
| `subject_panel` | int | right side of Home: 0 arm tiles, 1 installer mode card, 2 code pad, 3 alarm memory, 4 walk test |
| `subject_tone_color` | color | tone of the state (ring, icon, rail logo, saver dot) |
| `subject_alarm_active` | int | an alarm is sounding: red screen and card |
| `subject_hero_icon`, `subject_headline`, `subject_detail`, `subject_hero_note` | string | the state in icon and words |
| `subject_countdown`, `subject_countdown_pct` | int | exit/entry seconds left and ring fill (0..1000) |
| `subject_disarm_title`, `subject_disarm_note`, `subject_memory_note` | string | texts of the action tiles |
| `subject_pad_title`, `subject_pad_hint`, `subject_pad_digits`, `subject_pad_error`, `subject_pad_color` | mixed | code pad |
| `subject_pad_dots_on` | int | 1 = the dots show in the hint's place (digits typed, no error) |
| `subject_pad_glyph`, `subject_pad_glyph_on`, `subject_pad_note`, `subject_pad_note_on` | mixed | the icon left of the pad's title, and the plain words it carries over the screen (UI-025) |
| `subject_pad_close_on` | int | 0 hides the pad's X (the Disarm pad that stays on Home from the exit time on, UI-036) |
| `subject_modal_pad` | int | 1 = the code pad is open over the view you were on (chime, bypass, acknowledge, change your code) |
| `subject_chime_icon` | string | bell or crossed-out bell: the rail's Chime shows the state |
| `subject_pad_options`, `subject_opt_instant`, `subject_opt_silent`, `subject_opt_silent_avail`, `subject_opt_bypass`, `subject_opt_bypass_avail`, `subject_pad_silence` | int | arming options (silent exit for Away only) and Silence |
| `subject_locked` | int | armed or in alarm: Settings, Chime, codes and acknowledging troubles are locked (UI-022) |
| `subject_date`, `subject_time`, `subject_ampm`, `subject_wifi_icon`, `subject_wifi_ok` | mixed | status bar |
| `subject_battery_icon`, `subject_battery_text`, `subject_battery_critical`, `subject_battery_warn` | mixed | the status bar's battery: icon, percentage, red (absent or no valid reading) or amber (low) (PWR-011) |
| `subject_trouble_text` | string | the active troubles in words (screen saver) |
| `subject_trouble_count`, `subject_open_count`, `subject_chime`, `subject_default_codes` | int | badges and banners |
| `subject_zones_note`, `subject_zone_filter`, `subject_events_note`, `subject_troubles_note`, `subject_troubles_empty` | mixed | list views |
| `subject_zones_empty` | int | 1 = the zone filter has nothing to list: the green check and its phrase (`zones_none`) stand in for the list (BL-177) |
| `subject_troubles_pager`, `subject_troubles_page` | mixed | 1 = more troubles than one page, and which ones are on screen ("49-61 of 61", UI-030) |
| `subject_panic_report` | string | Emergency: whether anything is set up to carry the alarm further, and whether it is reachable (UI-029) |
| `subject_toast`, `subject_toast_visible` | mixed | toast |
| `subject_bar_banner` | int | status bar left side: 0 date, 1 factory-code banner, 2 installer-mode banner, 3 remote keypad (UI-039), 4 walk test, 5 an automation's arming, 6 an update runs (blue), 7 going back to the old version (amber; UI-021) |
| `subject_remote_banner` | string | the red remote keypad banner: who, and tap to pause (UI-039) |
| `subject_settings_section`, `subject_settings_pad`, `subject_page_title`, `subject_page_note`, `subject_page_back`, `subject_busy` | mixed | Settings: section, code pad shown, page header, spinner |
| `subject_sheet`, `subject_sheet_title`, `subject_sheet_note`, `subject_sheet_ok`, `subject_sheet_cancel` | mixed | the open sheet (1 keyboard, 2 choices, 3 confirmation) and its texts (`_cancel`: the confirmation's cancel caption) |
| `subject_wiz_step`, `subject_wiz_icon`, `subject_wiz_title`, `subject_wiz_note`, `subject_wiz_next`, `subject_wiz_next_on`, `subject_wiz_back`, `subject_wiz_exit`, `subject_wiz_pad` | mixed | first-boot setup (`_exit`: "Leave setup", a re-run only) |

String subjects hold at most 127 bytes (`UI_SUBJECT_STRING_LENGTH`): keep notes and dialog texts
shorter.

The event callbacks named in the XML (`ui_on_arm`, `ui_on_pad_key`, `ui_on_panic`...) live in
`alarm_ui.c` and forward to the action handler `ui_app.c` installs; without it (editor preview)
the buttons do nothing.

## Backend (`ui_backend_t`)
The UI's only way to the alarm system: status snapshots and their change counter, the
configuration (names), recent events, engine time and wall clock, `command()` (blocks until the
engine answered), `change_code()`, network state, key click and error beep, backlight. M4 added
(each optional: a NULL entry hides what needs it): `authenticate`, installer mode
(`program_enter/exit/keepalive/set_code`), `set_config`, `user_set/update/delete`, the event
log (`log_read`, `boot_id`), Wi-Fi (`wifi_info/scan/results/connect`), time zone and time server,
`save_options` and `set_volume` for this keypad's preferences, `about`, `setup_done` and
`factory_reset`. M11 added the zone sources (`zone_sources`, `zone_learn`, `zone_candidates`,
`zone_bind`, `zone_unbind`, `zone_info`; NULL keeps the zone editor without source rows). Later
features added their own entries (storage, notifications, remote access, the clock, Z-Wave...):
the whole contract is `components/core/ui_backend` (`ui_backend/ui_backend.h`). On
the panel it is `ui_port_esp.c` (alarm_service, local keypad source 0, zone_service); in the
preview it is the engine in-process with a RAM event log, the real zone hub and a simulated
Wi-Fi radio; on a remote keypad (M7) it will be the panel link client.

## Open questions
- Voice prompts on transitions ("Armed away, exit now")? See
  [08](../08-Audio-and-Annunciation/README.md): the voice announcements' policy (BL-044, after the
  voice is chosen, BL-015).
- Multi-partition UI: a partition switcher on Home (v2).
- Light theme.
- The alarm ring's slow fade stays (UI-020): a fade under once a second, not a flash; it
  is kept on the alarm. A steady ring was considered mainly for speed, when the panel was slower.
- Decided in M5 (UI-019, UI-027; grouped by UI-034): integrations get their
  own Settings section, Integrations, and are set up there only - Home Assistant, MQTT, ntfy and
  Pushover, each on its own page.
- Language support: English only, and the fonts ASCII-only, until
  [Globalization](../37-Globalization/README.md) (docs/features/37, M22). Since its G1
  every word in the `ui_*.c` modules and the XML goes through the catalog
  (`TR()`, the rules in `ui_internal.h`); next the fonts gain Latin-1 and Latin Extended-A, and a
  language and region setting joins Settings and first-time setup (BL-003, BL-004).

## Tasks
- [x] LVGL Pro project, design system, fonts and icons (M3)
- [x] Home, arm options, code pad, exit/entry, alarm/fire/CO, alarm memory, walk test (M3)
- [x] `ui_app`: snapshots -> subjects; taps -> commands; change code (M3)
- [x] Zones (bypass), troubles (acknowledge), activity, emergency, screen saver (M3)
- [x] PC preview + screenshots in CI (M3)
- [x] First run on the panel: touch responds; display refresh steady at 55.9 frames/s
- [x] Setup buttons that ignored taps and erratic scrolling: a touch-driver problem, fixed in
  [06](../06-CrowPanel-P4-Platform/README.md) finding 3; the preview walks the setup
- [x] On the panel: in daily use; no flashing since the fix
- [ ] Colours on the IPS panel, frame rate of the countdown ring, visible tearing: note what
  looks off while using it (BL-024)
- [x] Lockout countdown on the pad (BL-052, 5153415)
- [x] Settings, users, event log, about, installer mode, first-boot setup, keyboard (M4)
- [x] About > Software update (M6)
- [x] About holds System details and ends with Installer (BL-189, 032dde5)
- [x] The update server and the channel in Installer > Updates, and a code for Software update (BL-186)
- [x] An update's progress, Stop and the status-bar banner (BL-188, 7f10091)
- [x] Screen saver with a photo and a clear state card; saver brightness setting
- [x] Screen off and night hours, at the panel and in the Panel Manager (UI-043)
- [x] The first requests: the saver waits after every state change (UI-009); locked while
  armed (UI-022); quiet Instant exit (UI-004); a wording pass over every screen
- [ ] Try the 2026-09-19 requests on the panel: arm Instant, arm Away with a long exit
  time, tap the gear and Chime while armed (BL-024)
- [x] Zone editor: where each zone's sensor is, the source picker and the live terminal picker
  (UI-023, M11)
- [ ] On the panel: add a wired zone on the touchscreen; `display watch 60` while the terminal
  picker is open (BL-029)
- [x] Lists scroll from any touch: scrolling boxes are `clickable` and marked `user_1` (UI-024);
  the preview drags with a real moving finger; Zones, Troubles and settings column have no check
- [ ] Try the scrolling on the panel: About, System options and Activity, dragging from the
  text and from the buttons, with key clicks on (BL-024)
- [x] The external review's four UI findings: Emergency wording (UI-029), forgotten code entry
  (UI-032), toast layering (UI-031), paged trouble list (UI-030)
- [ ] Try the review's four fixes on the panel: the Emergency screen's second line with Home
  Assistant up and down, a code left half-typed during an alarm, and a source outage that
  troubles many zones (BL-024)
- [x] "Leave setup" on every step of a re-run of the setup (UI-033)
- [ ] Try it on the panel: installer menu > Run the setup again > Leave setup (BL-024)
- [ ] See the display-safety and remote-control fixes on a panel, as each report's "Verify"
  section says: a remote press cut off mid-hold that
  completes nothing ([B004](../../bugs/B004-remote-press-completed-panic-hold.md)) and the
  shield on Home in installer mode held at a second keypad, once there is one (M7;
  [B008](../../bugs/B008-installer-mode-state-icon-blank.md), checked in the preview) (BL-024)
- [x] The text sheet's steady cursor seen on a panel ([B003](../../bugs/B003-text-cursor-blinked.md))
- [ ] Screen saver pictures from the SD card (the decoder in `ui_saver.c` is ready for it)
  (BL-060)
- [ ] Cheaper glow on the Home state disk: the exit/entry ring redraws cost about 38 ms each,
  17 times a second (measured on the panel), which slows touch reading during entry time
  (BL-059; measured by 2026-09-25, when the panel was slower than now - measure again with
  `display watch` during an exit time before any work, and drop it if touch keeps up)
- [ ] Refresh `screens/` from the preview: the default walk now ends with three B014 bypass
  shots (`rf_bypassed_closed`, `rf_bypassed_open`, `rf_unbypassed_open`) that aren't here yet;
  and decide whether the default preview, and this gallery, take in the Z-Wave pages now that
  the P4's builds turn Z-Wave on (2026-09-27) while the preview still needs `-ZWave`
  (BL-088)
- [x] The touchscreen mockup and its proposal process, checked in CI (UI-044, 60afff6)
- [x] English slips kept byte-identical while the strings were marked for translation:
  singulars, "Codes are never shown", armed-note separator (docs/features/37 G1) (BL-109, 4de7cc1)
- [x] The rail holds places (Security, Zones, Activity, Panic); Chime is a round switch and the
  trouble count a pill in the status bar; the setup's last button says Done (BL-131 finding 26,
  d6b944e)
- [x] A password's field ends in an eye that shows what was typed (UI-016, BL-131 finding 27,
  d6b944e)
- [x] Settings in one-line sections, Installer apart, About's tag while an update waits; Time and
  language one page with one Clock choice and the clock's row (UI-015, UI-042, BL-131 findings
  1, 2, d6b944e)
- [x] Integrations say each state once: the rows without a paragraph, one switch per service
  with its state in its detail, the pairing code big with No and Yes side by side, ntfy (and
  MQTT, Pushover) in the order people use them, no library name (BL-131 findings 8, 13-16, bfcf4b6)
- [x] The clock set by hand with a calendar and hour, minute and AM/PM rollers on one sheet,
  in Time and language and the setup (UI-042, BL-131 finding 3, 2cc8ab8)
- [x] About this panel: the version and ID on top, Software update, the SD card and System
  details; the Storage and Internal flash pages gone, "Firmware space" in System details
  (BL-131 findings 9-12, 8e7fde1)
- [x] Wi-Fi and network: the joined network one row that opens its page, Forget at that page's
  bottom, "IP address settings", "DNS server", "Automatic" (BL-131 findings 5, 6, 3df5e39)
- [x] A user's page with browser sign-in a switch and Delete last; every picker as tall as its
  choices (BL-131 findings 17, 20, d66cabf)
- [x] The installer's menu in four groups, a zone's page grouped with its state on top, its
  sensor settings with pickers for choices (BL-131 findings 18, 19, 23-25, 2104b87)
- [x] Screen and sound in two groups without the chime, Night hours a page of its own with its
  hours on rollers (UI-043, BL-131 finding 4, c51b186)
- [x] The setup: "Leave setup" apart on the left, no stale toast, the codes row amber while a
  factory code remains (BL-131 findings 21, 22, 5517778)
- [x] The small fixes mockup-next lists: no "Setup finished" toast, the pad's keys dimmed while
  the keypad is locked out, a language named not coded, raw sensor states capitalised, a device's
  Supervision a picker, no page note repeating its section, "Lost the installer code?" a row of
  its own, when Software update last checked (BL-131, 49cbaed)
- [ ] The last of the approved redesign in mockup-next: Activity as the whole log, leaving Settings
  8 sections (finding 7, waiting on the maintainers' answer about LOG-008/LOG-010) (BL-131)
- [x] Light, High contrast and High contrast, light, and Automatic with the night hours, picked
  in Screen and sound and the Panel Manager (UI-010, BL-132, 1c72299, 22fa668)
