# 41 · Automations

**Status:** Building (2026-10-06): phases A, B and C built and host-tested - the automation core and its
service, the touchscreen's list, page and editors, the Panel Manager's pages, Z-Wave outputs as a device
source, arming and disarming by automation with Home's warning and the put-off, "By automation" on
Home's armed card; the mockups approved (M1-M3, MC); the Spanish drafted. Left: the Z-Wave outputs' Smart Home tiles
(MB, B3: Smart Home leaves them out until then) and the bench (U22, B4, C8) · **Milestone:** M25, own
track in three phases (A device automations BL-168, B the Z-Wave device source BL-169, C arming and
disarming by automation BL-170) · **Depends on:** 01, 03, 07, 14, 24 (the device registry), 31; coordinates with
16, 18, 26, 36

## Goal
The panel does a few things by itself, the way people expect from an alarm panel that also runs the
house: the porch light comes on when the front door opens in the evening, the doors lock when
everyone has left and the panel is armed Away, the lights come on when an alarm sounds, the water
main closes when a leak is found, and the panel arms Night at bedtime after a warning. These run on
the panel, so they work while Home Assistant is down - and, once the panel's own Z-Wave switches and
garage doors are a device source (phase B), without Home Assistant at all.

It is deliberately not a rules engine (docs/features/24 "Built-in automations, not a rules
engine"): one trigger, one or two actions, no conditions beyond an "Only between" window. Anyone who
wants more runs Home Assistant, which already sees every panel event (ADR-015, MQTT-010). This is
the one rule table for the panel: it delivers docs/features/24 HCTL-009/HCTL-011, the auto-arm core
of docs/features/18 (SCH-002 to SCH-005, SCH-008) and, once outputs exist, docs/features/16 OUT-005
(ADR-049). What an automation does is the homeowner's choice (ADR-050): it may disarm, unlock and open,
behind a plain confirm and an installer option.

## Reference behaviour
A keypad alarm panel drives its relays from output functions (a zone list, zone type or event
switches an output on, off, pulsed or for a time) and from timed events (relay on or off, arm Stay
or Away, disarm at a time); its auto-arm warns first and force-bypasses for Away. Panels sold as
"Security & Smart Home" keep on-panel automation small: one fixed local scene for an empty home,
set by the dealer, or "a system event runs a scene" at the panel, with richer scenes and rules
built in an app, not at the panel. The apps most people know have the rule we copy: a rule
started by locking or arming never runs anything that unlocks, disarms or opens. We first went
further (never disarm,
unlock or open, whatever the trigger); it was decided otherwise (D23): those are allowed, with a
plain confirm after a door, window, motion or alarm trigger and an installer option that can take
them off a site, and an automation's own arm or disarm never starts another. An arming automation
bypasses the zones it finds open only when told to (D29), as a keypad panel's auto-arm can; otherwise it
doesn't arm and says which zone was open.

On a panel without a screen (docs/features/42) automations are set up in the Panel Manager, and an
arming warning shows on the panel link's keypads with its Put off (docs/features/11 LINK-056, BL-222).

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| AUT-001 | An automation is one trigger and one or two actions, made at the panel or in the Panel Manager and run by the panel. At most 16 are stored. Each is shown as sentences built from its trigger and its actions ("When Front Door opens", "Turn on Porch Light for 10 minutes"), the same on the touchscreen and in the browser; it has no typed name | host tests, preview |
| AUT-002 | What an automation may do (D23), held at every layer. Device actions: turn a light or a switch on, off or toggle it, a fan on or off; lock or unlock; open, close or stop a garage door or a cover; open or close a valve; turn a thermostat off; run a scene. Alarm actions (phase C): arm a partition or disarm it (AUT-011). Never a script, an alarm panel or a siren; never panic or silence, and no bypass as an action of its own (an arming automation may bypass the zones it finds open, AUT-014). Disarm, unlock, open and stop are the less secure actions (`hc_auto_less_secure`): the installer option decides them (AUT-015) and a door, window, motion or alarm trigger needs a confirm for them (AUT-016). A scene, or a switch turned on or toggled, needs a one-time confirm when picked. The layers: the pickers; every save, from the touchscreen and the browser alike; a stored pair that isn't valid for its device never runs; the runner re-checks the device's live kind, the installer option and the person who saved it before each send; the registry's `hc_act_auto()`; the engine for the automation source (AUT-011) | host tests (each layer), preview walk |
| AUT-003 | Triggers react to real changes only. The first state after a start, an armed state restored at power-up, the settle time after a start, a configuration change, a walk test and installer mode start nothing; a zone in trouble, tamper or supervision trouble has not "opened". Silent alarms and duress never start an automation, and a duress arm or disarm starts exactly what a normal one would | host tests |
| AUT-004 | "Armed" fires when the partition becomes armed (exit time over, or at once without exit time), once per arming, at the level it ended at; never on entry expiry. "Disarmed" fires on a real disarm. "An alarm" and "a fire or CO alarm" fire once per alarm, never for a silent cause. "Opens" fires on a zone's fault for doors, windows, motion and water sensors (never smoke, heat, CO, gas, panic, keyfob, keyswitch or silent zones); "closes" for doors, windows and garage doors once closed for 10 s. A zone trigger runs at most once per 30 s | host tests |
| AUT-005 | "At a time of day" fires on the chosen weekdays at most once per local date. Nothing runs while the clock isn't set, and nothing missed runs late once it is set; a clock step of more than 10 minutes fires nothing, a smaller correction still runs a crossed minute; a minute skipped by daylight saving runs at the jump, a repeated one runs once; nothing catches up after a restart. An "Only between" window (any trigger but a time) waits while the clock isn't set | host tests |
| AUT-006 | A command once sent is never sent again (HCTL-012). A Home Assistant device not yet listed or offline is held for up to 30 s before it is sent; a busy device fails at once. The outcome (done, refused, no answer, unreachable, missing, busy, held back) is the automation's last-run status, kept until a restart. "Turn it off after" sends Off when its time is up; a new trigger restarts it | host tests, preview |
| AUT-007 | No loops and no strobing: at least 10 s between two automation actions on the same device, and between two automatic arms or disarms of the same partition; at most 12 actions a minute panel-wide; a device at most once per automation; an arming or disarming made by an automation never starts another automation's arm or disarm (it may start device actions: one hop) | host tests |
| AUT-008 | Automations are stored in their own NVS namespace, one record a key, a compat byte and TLV fields (DESIGN.md "Stored format"). A record with an unknown critical field or a newer compat is shown, read-only and never run; ignorable fields are kept and written back; a frozen fixture pins the format; the namespace is in backups; an add is refused when the NVS reserve would be used | host tests, fixture |
| AUT-009 | An arming or disarming by an automation is the engine's ARMED or DISARMED with source automation, never read as a person's (D30), anywhere: the event log and Activity say "By automation" and name which one by its trigger clause ("Disarmed · By automation · At 10:00 AM on weekdays"; the whole sentence where there is room), from the event's source id, the automation's uid (never reused); "By a deleted automation" once it is gone, and "By automation · changed since" for a line older than the automation's last edit. Home's armed card ("Since 10:30 PM · By automation"), the MQTT state, the Panel Manager's status and the Home Assistant link's "changed by" read the source first, so none names the person who saved it; that person is kept in the event and the JSON as `saved_by` (the engine acted as them). A bypass it makes, a bypassed zone watched again, an arming that bypassed ("1 zone bypassed") and every put-off ("Arming put off an hour · By Pat") are logged the same way. The one new event, `automation_failed` (trouble), is raised for a lock or a close that failed and for an arming skipped for want of a warning; an ARM_FAILED from an automation or a schedule is reported, with the open zone. A run is not logged | host tests |
| AUT-010 | The system master or the installer adds, changes, turns off and deletes automations (Settings > Automations; Panel Manager > Household > Automations); nobody changes them while armed or in alarm, and they keep running then. The server checks every save; the service console only lists them | host tests, preview, page tests |
| AUT-011 | Arming and disarming (phase C; decided 2026-10-05: the master decides, D4, D23): any trigger may start an automation that arms one partition Away, Stay or Night (its only action) or disarms one (alone or with one device action). It acts as the person who saved it, and the engine re-checks each time that this person may still arm or disarm that partition. It arms only from disarmed and never over another level; it bypasses zones only as AUT-014 says. A disarm is refused while the installer option is off (AUT-015) and while that partition's alarm sounds: an automation never silences an alarm (D22). The installer never saves one (BL-187, docs/features/03 USR-001): a save makes the saver the person it acts as, so the save is refused (`AUTO_ERR_NOT_ALLOWED`) and an installer's edit can't quietly stop a master's automation from arming | host tests |
| AUT-012 | Before an arming automation arms, whatever started it, it warns for N minutes (1-15, default 5): a steady status-bar banner, a card on Home with one "Put off" button (D26) and the chime once a minute; nothing flashes or counts down. The arm time is kept on the monotonic clock; without a whole warning there is no arming that day | host tests, preview |
| AUT-013 | Any user code at the panel puts the arming off (SCH-003), on every warning: "Put off" offers "An hour later" (the planned time + 60 minutes; the warning returns 5 minutes before) or "Not tonight" (no arming by this automation before 6:00 AM; "Not today", until midnight, for an arm time between 6 AM and 6 PM), then asks for a code (D26). "Not tonight" needs a code that may disarm that partition; another code gets only the hour ("That code can only put it off an hour") (D31). While it is put off, Home says so in one steady line - "Arms Night at 11:30 PM · put off by Pat", "Won't arm tonight · put off by Pat" (D27) - hidden while the partition is armed. Each put-off is an event (`arming_put_off`, by whom, which choice). A wrong code counts toward lockout; a duress code puts it off, looks the same and reports duress silently | host tests |
| AUT-014 | When a zone is open at arming time: with the automation's "Bypass open zones" (off by default; D29) it bypasses the burglary zones that are open (faulted) - a zone in tamper, loop or missing-sensor trouble still stops it - each bypass logged and reported "By automation", and arms, the ARMED marked "1 zone bypassed". A zone it bypassed is watched again once it has been closed for 10 s ("Kitchen Window watched again · By automation"), and every bypass ends at the next disarm. Fire, CO and 24-hour zones never block an arming and are never bypassed: they stay on whatever happens. Without it, or with a zone it may not bypass, it doesn't arm: the error sound, a reported ARM_FAILED naming the zone, and the reason in its status. During the warning, the card says plainly that the panel is not ready while a zone is open (D28; the zone screen has the details). A partition already armed is only noted in the status | host tests |
| AUT-015 | The installer option "Allow less secure automations" (on by default; the installer alone changes it) decides disarm, unlock, open and stop (D25). Turned off: the pickers don't offer them, a save holding one is refused, a stored one is kept as it is (never marked broken) but shows "Can't run" and is never sent, and the engine refuses an automation's disarm. Turned back on, they run again | host tests, preview |
| AUT-016 | A less secure action after a door, window, motion or alarm trigger is saved only after a plain confirm that says what it will do ("Anyone who opens Front Door will unlock Front Door Lock"); the server refuses the save without it, from the touchscreen and the browser alike, and the editor asks again whenever that trigger or that action changes. A time of day or disarmed trigger needs none (D24); after an armed trigger an unlock, open or stop asks it too, and a Disarm after an armed trigger of the same partition is refused at save: it could only undo the arming (D31; DESIGN section 3, from the research). Both editors also ask it before a Disarm of another partition after an armed trigger. A Disarm after an alarm or fire trigger is refused at save, whatever the option (D22) | host tests, preview, page tests |

## Decisions
| # | Decision | Decided |
|---|---|---|
| D1 | Its own track: this document, AUT- requirements, phases A/B/C (BL-168, BL-169, BL-170), ADR-049 | 2026-10-05 |
| D2 | Settings > Automations, the line after Integrations; it fits once Event log has moved into Activity (BL-131 finding 7, which was decided the same day: Activity becomes the whole log, LOG-008 kept) | 2026-10-05 |
| D3 | The system master or installer edits (`ACCESS_SYSTEM`, Panel Manager `ACC_SYSTEM`), like Integrations | design; approved with the mockups, 2026-10-05 |
| D4 | Arming automations: the master decides - no installer switch. The only action, a warning, put off once by any code, armed as the person who saved it; started by any trigger since D23 (first a time of day only) | 2026-10-05 |
| D5-D7 | Away, Stay and Night (Away warns "Inside sensors will be on"); put off by an hour (since D26: on every warning, or not tonight), no "Arm now"; a 1-15 minute warning with a steady banner, a Home card and the chime once a minute | design; approved with the mockups, 2026-10-05 |
| D19 | A coming automatic arming never takes the arming controls away: during the warning the Security tab's main buttons stay exactly as they are (not hidden, disabled or relabelled), on the touchscreen and on the Panel Manager's Overview, with the warning card above them (not in their place, as first drawn). Anyone can arm by hand at the level they want; the automation then finds the partition armed and does nothing (AUT-014) | 2026-10-05 |
| D20 | After a put-off the card and the banner go away; Automations says who put it off and until when ("Put off by Pat until 11:30 PM"), and the warning starts again 5 minutes before the new time (not a card kept for the whole hour, as first drawn); Home keeps a small note meanwhile (D27) | 2026-10-05 |
| D21 | The drawing's small choices stand: a plug icon for switches, a play icon for scenes (new to the icon font), "Something this software can't show" for a record from newer software, and any user's code puts an arming off | 2026-10-05, the mockups approved |
| D22 | The alarm comes first: an automation is a guest of the alarm panel. It never takes away, hides, disables, relabels or delays an alarm control (the Security tab on the touchscreen, the Panel Manager's Overview), a sound or a report; its task runs below the alarm's and the zones' (4 against 10 and 9) and never blocks them; its registration, its storage and its failures can't stop the boot or the engine; its chime yields to every alarm sound. A design, a mockup or a change that puts automations in the way of the alarm is wrong whatever else it does | 2026-10-05 |
| D23 | Automations may disarm, unlock and open (HCTL-011's ban lifted, ADR-050): unlock a lock; open, close or stop a garage door or a cover; open a valve; toggle a light or a switch; arm from any trigger, not only a time of day; disarm. The homeowner decides what the house does, even something unwise (a shop disarming at 10 AM on weekdays for a worker). What stays: only the system master or the installer makes automations (AUT-010); an alarm action, or a less secure one, acts as the person who saved it, re-checked each time; scripts, alarm panels and sirens never; never panic, silence or bypass, and never silence a sounding alarm (D22); no loops (AUT-007) | 2026-10-05 |
| D24 | A door, window, motion or alarm trigger may start a disarm, an unlock or an open after a plain confirm at save that says what it will do; the server checks it | 2026-10-05 |
| D25 | An installer option, "Allow less secure automations", on by default, covers disarm, unlock, open and stop: an installer can hold a site to the safer set. Off, those aren't offered, aren't saved and don't run; the engine refuses an automation's disarm | 2026-10-05 |
| D26 | The warning has one "Put off" button, not a row of choices; it asks for a code and then offers an hour later or not tonight, so after a party nobody keeps coming back to the panel | 2026-10-05 |
| D27 | While an arming is put off, people can see it: a steady note on Home and the Panel Manager's Overview, beside the partition, like the not-ready notice (who, until when or "not tonight") | 2026-10-05 |
| D28 | While the arming warning shows with a zone open, its card says briefly and plainly that the panel is not ready; the zone screen has the details (with "Bypass open zones" on, it says the open zones will be bypassed instead) | 2026-10-05 |
| D29 | An arming automation may bypass the zones it finds open and arm: its "Bypass open zones" choice, off by default, set by whoever makes it (master or installer). This reverses "never bypassing" (AUT-011, AUT-014; SCH-004's bypass half returns as this choice). Each bypass logged and reported as the automation's. From the research (DESIGN section 14): open zones only, never one in trouble, and each watched again once it closes. The installer option "Allow less secure automations" covers it (D31) | 2026-10-05 |
| D30 | An arming or disarming by an automation must not read as a person's: the log and Activity say an automation did it and name which one, so it can be tracked down | 2026-10-05 |
| D31 | Round 3: "Not tonight" needs a code that may disarm that partition, another code gets only the hour; "Bypass open zones" works only while "Allow less secure automations" is on - off, the editors don't offer the switch and the automation arms without bypassing, failing on an open zone, never "Can't run"; a Disarm after its own armed trigger is refused at save; an unlock, open or stop after an armed trigger asks the plain confirm; the no-silence limit stays (D22) | 2026-10-05 |
| D8-D9 | A scene, and turning a switch on, are allowed after a one-time confirm; scripts never | 2026-10-05 |
| D10-D16, D18 | "Only between" and "Turn it off after" in phase A; generated sentences, no typed names; "armed" when exit time is over; a new source kind `ALARM_SRC_AUTOMATION`; one failure event, runs not logged (since D30: a put-off is logged too); an unsent command held up to 30 s; 16 automations; a thermostat only turned off | design; approved with the mockups, 2026-10-05 |
| D17 | The Z-Wave device source is phase B of this track | 2026-10-05 |

## Design
[DESIGN.md](DESIGN.md) has the whole design: the triggers' exact semantics (section 2), the action
allowlist and the layers that hold it (3), the relationship with schedules, outputs, device sources
and notifications (4), the portable core `components/core/automation` with its API, the service
task and threading (5), the stored format (6), events (7), access (8), every touchscreen screen (9)
and the Panel Manager page (10), and the risks (13).

In short: a listener on the alarm task turns engine events and snapshot changes into occurrences
(`auto_watch`) and queues them to an `auto` task, which matches them against the automations
(`auto_run`), steps the local clock (`auto_clock`) and sends device actions through
`device_service_act_auto()` - or, in phase C, arms or disarms through
`alarm_service_command_as()` as the person who saved the automation. The core is portable C11 and runs
unchanged in the PC simulator.

## Interfaces and data
- Core: `components/core/automation` (`automation.h`, `auto_watch.h`, `auto_clock.h`, `auto_run.h`;
  DESIGN.md section 5). Service: `components/services/automation_service`. Feature switch
  `CONFIG_ALARM_FEATURE_AUTOMATIONS` (depends on `CONFIG_ALARM_HOME_CONTROL`, HCTL-010).
- Engine: `ALARM_SRC_AUTOMATION` (appended, "automation"), `ALARM_EV_AUTOMATION_FAILED`
  (`automation_failed`, appended); phase C `ALARM_CMD_CODE_CHECK`.
- Registry: `hc_auto_allowed()`, `hc_auto_less_secure()`, `hc_act_auto()`, `HC_ERR_NOT_ALLOWED`;
  phase B `DSRC_ZWAVE`.
- Configuration: the installer option `opt.auto_less_secure` ("Allow less secure automations",
  default on, AUT-015).
- Storage: NVS namespace `automations` (`meta`, `a01`..`a16`), DESIGN.md section 6.
- Panel Manager: `GET automations`, `GET automations/devices`, `POST automations` (DESIGN.md
  section 10).

## Test and acceptance
- Host: the core's tests tagged AUT-* (codec and fixture, validator, watch, clock, runner, an end to
  end test over `alarm_sim` enumerating every trigger and action pair), `home_control`'s HCTL-011
  tests, the engine's CORE rows for the source kind and the guard, the backup policy test.
- Preview: `walk_automations()` checks that unlock, open and disarm are offered only while the
  installer option is on, that a door trigger asks for the confirm, and shoots every screen; the
  en-XA walk shows nothing cut off.
- Panel Manager: the page tests per role, and the server refusing a script, a save without the
  confirm and a less secure action while the option is off.
- Bench (a flash only when told to): the porch light from a door, a lock on Away, a failed
  lock's notification, Home Assistant restarting within 30 s; phase C: Night at a set time, a
  put-off, a duress put-off, an open window, a time-zone change during the warning.

## Open questions
- Finding 7 (Event log into Activity) has to land before the Settings line (D2): the
  LOG-008/LOG-010 answer.
- Sunrise and sunset need a "where the panel is" setting and a library (DESIGN.md "Later").
- Automation-sent actions are unknown to the Smart Home tiles' pending words (DESIGN.md R8).

## Tasks
Units sized for one piece of work each (DESIGN.md had them as U, B and C numbers). The mockups are the
approval: screen and page units wait for them.

Phase A, device automations (BL-168):
- [x] M1 · Touchscreen proposal (now `mockup/screens/automations.js`) and its kit parts; approved (BL-168, bef2c08)
- [x] M2 · Panel Manager proposal (deleted; today's pages are `panel-pages.js`); approved (BL-168, bef2c08)
- [x] U1 · `home_control`: `hc_auto_allowed`, `hc_act_auto` returning the request id, `HC_ERR_NOT_ALLOWED`, the id in results, a dropped row's pending action failed; `device_service_act_auto` and a result listener (AUT-002, HCTL-011/012) (BL-168, bef2c08)
- [x] U2 · Engine: `ALARM_SRC_AUTOMATION` = "automation", the dispatch guard refusing every command from it, "By automation" (AUT-002, AUT-009) (BL-168, bef2c08)
- [x] U3 · `ALARM_EV_AUTOMATION_FAILED`: name, platform event, words, trouble category, JSON keys and golden payload; then ui_words_test, docs/features/10 and the Spanish words (AUT-009) (BL-168, bef2c08)
- [x] U4 · `components/core/automation`: scaffold, model, validator and codec with fixture v1 and the NVS cost pinned (AUT-001, AUT-002, AUT-008) (BL-168, bef2c08)
- [x] U5 · `auto_watch`: baselines, settle, walk test, troubles, the armed latch, alarm episodes, silent and duress, the zone allowlist (AUT-003, AUT-004) (BL-168, bef2c08)
- [x] U6 · `auto_clock`: the time rules T1-T7 with daylight-saving tables (AUT-005) (BL-168, bef2c08)
- [x] U7 · `auto_run`: matching, window, cooldowns, close hold, device gap, the panel-wide cap, sending, holding, outcomes, "Turn it off after", Try it now; an end-to-end test over `alarm_sim` (AUT-006, AUT-007, HCTL-009, HCTL-011) (BL-168, bef2c08)
- [x] U8 · Lift HCTL-011's ban (D23-D25): `hc_auto_allowed` with the installer option and `hc_auto_less_secure`, the Z-Wave source's open refusal removed, the validator's confirm and option checks, the maker kept for every automation and re-checked before a less secure action, `opt.auto_less_secure` in alarm_config (AUT-002, AUT-015, AUT-016) (BL-168, bef2c08)
- [x] U9 · `CONFIG_ALARM_FEATURE_AUTOMATIONS`, `automation_service` store (load outcomes, save lock, generation, NVS reserve) (AUT-008) (BL-168, bef2c08)
- [x] U10 · The `auto` task, the listener (`MAX_LISTENERS` 12, registration that can't stop the boot), ports, local time, console `automations` (BL-168, bef2c08)
- [x] U11 · Backup policy and its test; docs/features/14 storage and docs/features/20's NVS budget (AUT-008) (BL-168, bef2c08)
- [x] U12 · The simulator runs the core, with Harbor House automations (BL-168, bef2c08)
- [x] U13 · `ui_backend_t` hooks, the simulator's fakes and the panel's port (BL-168, bef2c08)
- [x] U14 · Layout XML: the Settings line, `time_sheet`, `day_toggle`, the plain confirm, icons; after M1 and finding 7 (BL-168, bef2c08)
- [x] U15 · `ui_automations.c`: the list and its states, the automation's page (BL-168, bef2c08)
- [x] U16 · The pickers and sheets (BL-168, bef2c08)
- [x] U17 · `walk_automations()`: HCTL-011 checks, shots, en-XA, Spanish (BL-168, bef2c08, d713f98)
- [x] U18 · `web_automations.c`, routes, access, capability (AUT-010) (BL-168, bef2c08)
- [x] U19 · The Panel Manager page, mock data and page tests (BL-168, bef2c08)
- [x] U20 · ha-addon: `automation_failed` (and `arming_put_off`) in its event types (the
      integration's repository) (BL-168, `ha-addon` 7b6671a)
- [x] U21 · Docs close-out: status lines, screens moved into the mockup, proposals deleted (BL-168, 952dd62)
- [x] M3 · Both proposals redrawn for D23-D25: unlock, open, stop and toggle in the pickers, the plain confirm, the installer option's row; approved (BL-168, bef2c08)
- [x] U23 · The installer option on the touchscreen and in the Panel Manager, after M3 (AUT-015) (BL-168, 998246c)
- [ ] U22 · Bench with Home Assistant (BL-168)

Phase B, the Z-Wave device source (BL-169):
- [x] B1 · Portable `dsrc_zw` over the Z-Wave outputs, `DSRC_ZWAVE` appended, `HC_MAX_SOURCES` 3 (BL-169, bef2c08)
- [x] B2 · Z-Wave commands posted with a result, registered before Home Assistant; `CONFIG_ALARM_HOME_CONTROL` also with Z-Wave alone (BL-169, bef2c08)
- [ ] MB · Smart Home proposal for Z-Wave tiles, approved (BL-169)
- [ ] B3 · Smart Home words and glyphs for Z-Wave devices; preview (BL-169)
- [ ] B4 · Bench: a Z-Wave switch from Smart Home and from an automation, a garage door closed by one (BL-169)

Phase C, arming by automation (BL-170):
- [x] MC · The arming screens in both proposals (now `screens/automations.js` and `home.js`), redrawn for D19, D20 and D23 (any trigger, Disarm), approved (BL-170, bef2c08)
- [x] C2 · Engine: the guard lets an automation arm (no flag but `BYPASS_FAULTED`) or disarm as the person who saved it, a disarm only while the installer option is on and never while that partition's alarm sounds; ARM_FAILED reported for automation and schedule sources with its zone; `ALARM_CMD_CODE_CHECK` with duress, answering whether the code may disarm that partition (AUT-011, AUT-013, AUT-014, AUT-015) (BL-170, bef2c08)
- [x] C2b · Engine: an automation's bypass takes open burglary zones only (a zone in trouble still fails the arm), marks the ARMED "bypassed at arming" and watches each zone again once it has been closed 10 s (AUT-014, D29) (BL-170, bef2c08)
- [x] C2c · Who did it (D30): `arm_source` in the partition snapshot, read first by Home's armed card, MQTT `armed_by` and the Panel Manager; `ALARM_EV_ARMING_PUT_OFF` appended with its words and JSON; `notify` names the automation by its trigger clause through a lookup, "By a deleted automation", the record's `changed_at` and "changed since"; MQTT `automation` and `saved_by` (AUT-009, AUT-013) (BL-170, 6ee89e5, e1f9f34)
- [x] C3 · `auto_arm`: the warning, the monotonic arm time, the whole-warning rule, the put-off on every warning (an hour, or not tonight until 6:00 AM / not today until midnight), statuses; one warning at a time; the disarm action; an automation's own arm or disarm starting no other, the partition gap; the validator refusing a Disarm after its own armed trigger (AUT-007, AUT-011, AUT-012, AUT-013, AUT-016) (BL-170, bef2c08, 5450969)
- [x] C4 · Service: the arm port, the warning mailbox, the chime, the code check (BL-170, bef2c08)
- [x] C5 · Home's banner, card and the put-off pad (BL-170, c9593fc, 3d4bbec, 1e660f4)
- [x] C6 · Arm in the touchscreen editor and in the Panel Manager, with their walks and tests (BL-170, 364a320, 5bc5ed7, 2bb0621)
- [x] C7 · Docs: docs/features/18's auto-arm boxes, this document (BL-170, 6ed50e8, 952dd62)
- [ ] C8 · Bench: Night at a set time, a put-off, a duress put-off, an open window, a time-zone change (BL-170)
- [ ] C9 · The reviews' follow-ups: one copy of the trigger clause (`notify_auto_when` repeats `ui_auto_when_text`); another partition's warning banner opens Home, which has no card for it; tests for the put-off answer without a clock (NO_CLOCK), a partition master as an automation's maker, a second warning's chime and the chime's timing; the Panel Manager's Disarm partitions taken from `state.partitions` (check it is the set the saver may disarm); the touchscreen's partition row offering a one-choice sheet; the walk deleting every Arm rule, not only its own (BL-170)
