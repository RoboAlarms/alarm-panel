# 41 · Automations: design

The design behind [README.md](README.md) (the requirements, the decisions and the tasks). It was
written from a read-only map of the repository, three independent designs and three reviews
(fit to the aims, correctness, buildability), merged, then changed by the decisions of
2026-10-05 (README "Decisions"). File and line references are as of fd96a38.

## 0. Summary

- **What v1 is.** A built-in, alarm-centric automation is one trigger plus one or two actions, run by the panel itself. Up to 16 can be stored.
- **Its own track.** It gets `docs/features/41-Automations` (requirement IDs `AUT-`) and three backlog rows, built library first:
  - **Phase A: device automations.** Every trigger in HCTL-009, plus a fixed "Only between" window and "Turn it off after". Actions are on, off, toggle, lock, unlock, open, close, stop, a thermostat off, and a scene or switch-on after a one-time confirmation. Unlock, open and stop sit behind an installer option and, after a door or alarm trigger, a plain confirm (D23-D25).
  - **Phase B: a Z-Wave device source.** This is what makes "works without Home Assistant" true.
  - **Phase C: arming and disarming.** Any trigger can arm the panel, set up by the system master or installer (no installer switch: decided 2026-10-05), after a warning that any code can put off once; it can also disarm, behind the installer option (D23, D25).
- **Where it lives.**
  - On the touchscreen it is Settings > Automations, the line after Integrations. The page is opened with the system master's or installer's code (`ACCESS_SYSTEM`).
  - In the Panel Manager it is Household > Automations, opened by the system master or installer (`ACC_SYSTEM`).
  - The PC simulator runs the same portable core (`components/core/automation`).
- **Decisions that matter most:**
  - An arming or disarming automation acts *as* the rule's maker through the existing `alarm_service_command_as()` (alarm_service.c:807-819). The engine refuses every other command from the new `ALARM_SRC_AUTOMATION`, and a disarm while the installer option is off or while that partition's alarm sounds.
  - "Armed" fires when exit time ends. It never fires on entry-delay expiry, and never twice for one arming.
  - Logging is minimal. There is one new event, `automation_failed` (TROUBLE, only for a lock or close that failed, plus phase C's skipped arming). Arming failures are reported by making `ARM_FAILED` REPORT_NOW for automation and schedule sources. Runs are never logged.
  - The Settings column needs finding 7 (Event log moving into Activity) to land first, or the list scrolls.
- **What can start at once.** The portable core and its host tests, the engine and home_control changes, and both mockups. The screens wait for the mockups' approval.

## 1. Scope (v1 vs later, with reasons)

**Track.** The feature gets its own track: `docs/features/41-Automations`, requirement prefix `AUT-`. The backlog rows come from `backlog.py next-id` (assumed BL-168 A, BL-169 B, BL-170 C). A new ADR (next free number) makes 41 the single rule table for 24's HCTL-009, 18's auto-arm (SCH-002..005) and 16's OUT-005. HCTL-009 and HCTL-011 stay in 24 as the umbrella and point to 41. This applies the "big features get their own track" rule: it has a stored format, engine contracts, two UIs and 40-odd units.

| Phase | Ships | Why it stands alone |
|---|---|---|
| **A** (BL-168) | Triggers: armed (Away, Stay, Night, any), disarmed, an audible alarm, a fire or CO alarm, a door or window or sensor opening, a door or window closing, a time of day on chosen days.<br>Actions: light, switch or fan on or off, a light or switch toggled; lock or unlock; cover, garage or valve open or close, a cover stopped; thermostat off; scene run.<br>Options: "Only between" and "Turn it off after".<br>Surfaces: touchscreen, Panel Manager, simulator. | Useful at once with Home Assistant devices. It needs no other feature and adds only two engine contracts: the source kind and one event. |
| **B** (BL-169) | `DSRC_ZWAVE`: the panel's own Z-Wave switches, dimmers and garage doors (close only) appear in the picker and on Smart Home | The only devices that work with Home Assistant absent (HCTL-009's promise). It runs as a parallel track. |
| **C** (BL-170) | The Arm action from any trigger, added by the system master or installer, with a warning, a put-off on every warning (an hour, or not tonight), "Bypass open zones", failure reporting and the Home card; the Disarm action behind the installer option; the automation named wherever its arming or disarming shows (D30). | It delivers 18's auto-arm core once. A rejected arming design costs phase A nothing. |

**Requirements**: AUT-001 to AUT-016, in [README.md](README.md).

**Later, with reasons:**

| Later | Reason |
|---|---|
| Sunrise and sunset ("after dark") | The panel stores no location, only a wide POSIX zone (ui_tz.c:18). It needs a "Where the panel is" setting plus a popular library, e.g. Paul Schlyter's public-domain `sunriset.c` (licence checked when picked). The fixed-hours window covers v1. |
| Typed names | The generated sentence is always true and is the same on the panel, in the browser and in Activity. Field id 0x80 is reserved. |
| Thermostat heat, cool or setpoint; brightness | The HA catalog carries no setpoint or mode attributes (ui_port_esp fills unknown temperatures). |
| Scripts | Never: they are opaque code. |
| Relays (OUT-005) | Blocked on BL-037 hardware. They arrive as `AUTO_ACT_OUTPUT = 3` in the same table. "Pulse" and "follow" stay output functions. |
| Partition masters editing | Doors and lights cross partitions. Filtering adds states a homeowner must learn. |
| Exception dates, scheduled users, auto-disarm | They stay in feature 18 (SCH-001/007/006). Since D23 an automation may disarm, so phase C can deliver auto-disarm too. |
| Entry-delay, trouble and device-state triggers; notification actions | Each is an appended enum value in an unchanged format. Feature 26 already notifies every event. |
| "Remind to arm" | The commonest commercial rule (an arming reminder, "late to close", "please close now"): the warning card and chime only, when the partition is still disarmed at that time, and no arming. An appended action using what C builds. |
| A zone's own "Never bypass" | A per-zone option on classic panels. Today every burglary zone can be bypassed (config.c:228-237); a zone option would keep a chosen door out of "Bypass open zones" too. |
| Put off from Home Assistant or a phone | HA's actionable notifications already do it with a code and its own authentication; a phone button that acts without a code is the risk (section 14). |

**Never:**
- scripts;
- panic, silence or a bypass of its own (an arming automation's "Bypass open zones" is part of its arming, D29), and a disarm while that partition's alarm sounds (D22);
- `alarm_control_panel` or `siren` entities;
- a "flash the lights" action (display safety);
- condition chains (24:105-114).

## 2. Triggers

**How occurrences are produced.** A portable observer, `auto_watch`, runs inside the automation listener on the alarm task. It sees every snapshot: each zone message is its own engine call (alarm_service.c:427-471), so a fast open-then-close is never lost. It turns engine events and snapshot changes into occurrences (`auto_occ_t`) and sends them to the automation task with `xQueueSend(…, 0)`. The runner (`auto_run`) matches occurrences against the rules on that task.

**Rules for every trigger:**
- **B1 Boot baseline.** The first snapshot after start is a baseline. A restored armed state at power-up emits no ARMED event (engine.c:219-264) and makes no occurrence here.
- **B2 Settle.** Zone occurrences are ignored until start + `cfg.opt.power_up_settle_s` (default 60). The engine's `settle_until` is private (zones.c:453-455), so the module keeps its own grace and the engine doesn't change. The first snapshot after the grace becomes the zone baseline.
- **B3 Re-baselining.** A change of `snapshot.config_seq` re-baselines every zone. `on_zones_reset` re-baselines the zones it names. The automation task then reloads zone types.
- **B4 Walk test and installer mode.** No zone occurrences while `snapshot.program` is set, or while any partition has `part[p].walk_test`. A walk test in any partition holds every zone, because the snapshot carries no zone partition; zone occurrences carry partition 0 and the runner reads the zone's partition from the configuration. Zones still show FAULT then (zones.c:310-318). The zones re-baseline when it ends.
- **B5 Trouble.** A zone edge is dropped when the old or new flags carry `ALARM_ZS_TROUBLE|TAMPER|SUPERVISION` (alarm_types.h:176-182): a sensor that went missing hasn't opened.
- **B6 Queue overflow.** The listener never blocks. On overflow it counts `missed`, and the console shows the count.
- **Silent and duress.** An `ALARM_EV_ALARM` carrying `ALARM_EVF_SILENT` never fires anything. `ALARM_EV_DURESS` is never a trigger. A duress arm or disarm fires exactly like a normal one (arming.c:194-198, 270-273), so nothing observable reveals a duress code.

| Trigger (stored `auto_trig_t`) | Fires on | Partition and filters | Repeat |
|---|---|---|---|
| `ARMED`=1 (levels mask; 0 = any) | **The snapshot's transition into ARMED** from DISARMED (zero exit time: eng_arm goes straight to ARMED, arming.c:184-192; relaxed_limits allows exit 0, config.c:357-364) or from EXIT_DELAY (the end of exit time, arming.c:398-401). **Never from ENTRY_DELAY**, which is entry expiry going into alarm (arming.c:404-411). An exit error goes EXIT_DELAY → ENTRY_DELAY → alarm, and gives no ARMED. The level is read at that moment, so keypad Away that AUTO_STAY turned into Stay (arming.c:370-379) fires "armed Stay". | Partition mask (0 = any enabled) | **Once per arming.** A per-partition latch remembers the level it fired for and clears on DISARMED. An exit restart (ARMED → EXIT_DELAY → ARMED, arming.c:321-331) therefore does not run it again. Re-arming Stay → Away fires Away. |
| `DISARMED`=2 | `ALARM_EV_DISARMED`, emitted only on a real change (arming.c:231-316), exit and entry time included | Partition mask | Once per event. Not on SILENCED, WALK_TEST_END or MEMORY_CLEARED. |
| `ALARM`=3 | `ALARM_EV_ALARM`, no `ALARM_EVF_SILENT`, cause in the allowlist BURGLARY, EXIT_ERROR, TAMPER, 24H_AUDIBLE, PANIC_POLICE | Partition mask. SILENT_BURGLARY, 24H_SILENT, PANIC_SILENT, 24H_AUX and PANIC_MEDICAL are refused by cause even if a flag were missing. | It fires when the alarm is raised (on_event is immediate; the report waits for the abort window, events.c:41-90). An exit error's siren starts at the exit error, but `ALARM_EV_ALARM`, and so the occurrence, comes only when entry time expires. An event without a partition 1..`ALARM_MAX_PARTITIONS` makes no occurrence. **Once per episode**: a per-partition latch clears when the snapshot shows the partition DISARMED and its sounder not on ALARM, FIRE, CO or AUX. |
| `FIRE`=4 | `ALARM_EV_ALARM` with FIRE, PANIC_FIRE or CO. A verified fire zone fires only on its confirmed second trip (zones.c:215-238). | Partition mask (0 = any) | Same episode latch |
| `ZONE_OPENED`=5 | `ALARM_ZS_FAULT` 0→1 | **Type allowlist** (alarm_types.h:104-126): 1, 2, 3, 4, 5, 8 (24-hour aux, used for water and temperature; it sounds at the keypad and is never silent), 10, 12, 23. Any future type is excluded until added. **Class exclusions**: smoke, heat, CO, gas, panic button, keyfob, keyswitch. Bypassed and swinger-shut-down zones still fire, because the door's state is real. Re-checked at run time against the live configuration. | 30 s cooldown per rule. A repeat inside it only restarts a "Turn it off after" timer. |
| `ZONE_CLOSED`=6 | FAULT 1→0, and still closed **10 s later** (`AUTO_CLOSE_HOLD_MS`, kept on the automation task) | Door, window and garage-door classes only, same type allowlist | A re-open inside the 10 s cancels it. The "Only between" window is checked at the close; the 30 s cooldown starts when the hold ends. |
| `TIME`=7 (minute 0..1439; days bit0 Mon..bit6 Sun, 0 = every day) | Local wall-clock time crosses the minute on an allowed weekday | No partition (an Arm action names its own) | **At most once per local date** (`last_run`, in RAM) |

**"Only between" window** (any trigger except TIME):
- Stored as from and to minute+1, so 0 means none. It wraps past midnight, and from == to is refused at save.
- It is checked at the trigger. While the clock isn't set, a windowed automation doesn't run, and its status says so.

**Time rules (AUT-005).** The platform port fills `auto_local_t {valid, epoch_s, day_no, minute, weekday 1=Mon}`.
- `valid` = epoch ≥ 2025-01-01 and `ALARM_TRBL_CLOCK_NOT_SET` clear (alarm_snapshot.h:22).
- `day_no` comes from `rtc_epoch_from_civil(&civil, &epoch)/86400` on the local civil date. The real signature takes a `const rtc_civil_t*` (ds3231.h:77).

`auto_clock_step` compares each tick (at most 1 s apart while any time rule exists) with the previous one:

| Rule | Situation | What happens |
|---|---|---|
| T1 | Clock not set | Nothing runs. The first valid tick is a baseline, so nothing missed runs late (SCH-008 analogue). |
| T2 | Step or stall: \|Δepoch − Δmono\| > 10 min, or Δmono > 10 min | Re-baseline and fire nothing. Covers an SNTP step at 23:30 for a 23:00 rule. |
| T3 | Small correction: a forward step of 10 min or less | The crossed minutes fire, so a 95 s SNTP correction across 23:00 still runs the rule. |
| T4 | Normal progress | Rules whose minute lies in (previous, now] fire: spans are the inclusive minutes [from, to], previous + 1 to now. Across midnight the interval is split per date, each with its own weekday. |
| T5 | Spring forward (EST5EDT, 2026-03-08) | Local time jumps 60 min with no epoch jump. The skipped minutes fall inside the interval, so a 02:30 rule fires once, at 03:00. A local advance beyond Δepoch + 61 min is treated as a time-zone change and re-baselines. |
| T6 | Fall back, or a backward step | The interval is empty. `last_run` stops a second 01:30 on 2026-11-01. A backward step of more than 10 minutes forgets the runs after the new reading, so a clock once set wrongly into the future can't silence rules (see the run memory below). |
| T7 | Restart | All state is RAM-only and the first tick is a baseline: no catch-up, no double run. |

**The run memory.** Each time rule keeps `last_run` = day_no × 1440 + minute of its latest run (`AUTO_RUN_NONE` = never). It is due only when floor(`last_run` / 1440) isn't the span's date. When the epoch goes more than 10 minutes below the latest epoch seen (a T2 step backwards, a day or more included, also across a stretch without a valid clock), `auto_clock` bumps its generation and records the floor: the new reading's day_no × 1440 + minute. The caller then forgets every `last_run` greater than the floor, so a clock set ahead by 2 h or 20 h and corrected still runs the real 08:00; runs at or before the floor stay. Fall back (the epoch moves forward) and a backward step of 10 minutes or less bump nothing: a rule that ran 3 minutes early doesn't run again that date.

No new `net_wifi_tz_gen()` contract is needed: T5's bound catches zone changes larger than an hour. A change of exactly one hour behaves like DST (documented).

**Arming and walk tests.** Arming triggers can't occur in a walk test or installer mode, because the engine refuses. Alarm and fire triggers still work in installer mode, because fire is 24-hour.

## 3. Actions

**What an automation may do** (D23) is one portable predicate, `hc_auto_allowed(kind, action, value, mode, less_secure)` in home_control. It is true only when `hc_action_valid`, a mode word comes only with `ACT_SET_MODE` (ha_link sends `mode` with any action), the pair is in this table, and, for a less secure pair, `less_secure` is true. `hc_auto_less_secure(kind, action, value)` names those pairs: `hc_needs_code` (unlock; a cover or valve opened, or a cover set short of closed) and a cover's STOP (it can leave a door part-open). An automation asks for no HCTL-005 code: it acts as the system master or installer who saved it.

| `dev_kind_t` | Offered | Less secure: only with the installer option (D25) | Never, or later |
|---|---|---|---|
| LIGHT | Turn on (`ACT_ON`, **value −1**: 0 is brightness 0, dsrc_ha.c:249-250), Turn off, Toggle | — | SET_LEVEL (later) |
| FAN | Turn on (value −1), Turn off | — | SET_LEVEL (later) |
| SWITCH (input_boolean too) | Turn off. Turn on and Toggle only with the action's `confirmed` flag (D9). | — | — |
| LOCK | Lock | Unlock | — |
| COVER (garage doors) | Close (`SET_LEVEL 0` is normalised to CLOSE) | Open, Stop | SET_LEVEL > 0 (later) |
| VALVE | Close | Open | — |
| CLIMATE | Turn off (`ACT_SET_MODE` "off", D18) | — | Heat, cool, auto, SET_TEMP (later) |
| SCENE | Run, only with `confirmed` (D8) | — | — |
| SCRIPT | — | — | RUN, always: a script can do anything, unseen |
| `AUTO_ACT_ARM` (phase C) | Away, Stay, Night; flags 0, or `BYPASS_FAULTED` when its "Bypass open zones" is on (D29); any trigger; the automation's only action | — | Panic, silence, a bypass on its own, NO_ENTRY_DELAY, SILENT_EXIT |
| `AUTO_ACT_DISARM` = 4 (phase C) | — | Disarm one partition, alone or with one device action | While that partition's alarm sounds (D22) |

**The confirm (D24, AUT-016).** A less secure action after a ZONE_OPENED, ZONE_CLOSED, ALARM or FIRE trigger is saved only with that action's `confirmed` flag; without it `auto_rule_check` answers `AUTO_ERR_NEEDS_CONFIRM`, and the editor shows a plain (blue) confirm when the pairing forms, saying what it will do ("Anyone who opens Front Door will unlock Front Door Lock, even when the alarm is on", "Anyone who opens Front Door will disarm Home") with **Use it** and **Cancel**, like the scene's confirm. The editor clears the flag whenever the trigger or that action changes, so each new pairing is confirmed again; the server only checks that it is set. A time of day or disarmed trigger needs none: those are a schedule or follow a code. After an armed trigger an unlock, open or stop asks it too (D31): arming Away is often leaving, and the confirm names it ("Every time Home is armed Away, Garage Door will open").

**The installer option (D25, AUT-015).** `opt.auto_less_secure` in alarm_config ("Allow less secure automations", default on, installer only, docs/features/14). It never changes what is stored: a less secure action saved while it was on stays as it is, shows "Can't run" while it is off, and runs again once it is back on.

**The layers, each tested (AUT-002, AUT-015, AUT-016):**
1. **Pickers.** Both pickers are built from `hc_auto_allowed` with the option, and the web gets `actions[]` from the server. With the option off no Unlock, Open, Stop or Disarm row exists. A preview `expect()` checks both ways.
2. **Save.** `auto_rule_check()` runs in `automation_service_save()` before any NVS write, from the touchscreen and the web alike: the pairs, the option, the confirm and the maker.
3. **Decode.** A stored pair that isn't valid for its kind, or a script, makes the record `AUTO_REC_BROKEN`: kept, shown, never run (fixture h). The option never does (above).
4. **Run.** Before each send the runner re-checks the live registry kind (`device_service_get`), the option and, for a less secure action, that the person who saved it is still a system master or installer (`maker_ok`). A ref that now names a lock instead of a light is refused with the status "Can't run: open it and pick the device again". It also re-checks zone types.
5. **Registry.** `hc_act_auto(..., less_secure, ...)` returns `HC_ERR_NOT_ALLOWED` outside the table. It is host-tested beside `test_needs_code` (test_home_control.c). `device_service_act_auto()` is the only device call the service makes.
6. **Source** (phase B). The Z-Wave source runs what the registry passed; it no longer refuses an automation's open. `zwz_panel_ha_command`'s refusal of a garage open (zwz_panel.c:860-863) is Home Assistant's own path and stays.
7. **Engine.** At the top of `dispatch` (arming.c:694), a command whose source is `ALARM_SRC_AUTOMATION` returns `ALARM_ERR_NOT_AUTHORIZED`:
   - **Phase A:** always.
   - **Phase C:** unless it is `ALARM_CMD_ARM` with `arm_flags == 0`, or `ALARM_CMD_DISARM` while `opt.auto_less_secure` is on and that partition's alarm isn't sounding, run `as` a user (the maker).

   This keeps the code-less PANIC and SILENCE paths (arming.c:720-745) closed: an automation can never silence a fire bell.
   The guard covers commands, the only engine calls an automation makes. The other entry points that take a source
   (`authenticate`, `user_set*`, `program_enter*`) have none, and the service never calls them for an automation.
   Like any refusal it plays the error sound (CORE-USR-029).

**What the panel can't see.** A scene may unlock or open, and a switch may front a gate opener. Both need the owner's attestation when picked (section 9), and the installer option can't reach inside them: the add-on cannot see inside `scene.turn_on`, so no integration-layer claim is made.

**Sending and outcomes (AUT-006, HCTL-012).** Once sent, a command is never sent again. Waiting to send a command that never left is not a replay.

| At `hc_act_auto`, then afterwards | Sent? | Handling and status reason |
|---|---|---|
| `HC_OK`, then result ok | Yes | Done |
| `HC_OK`, then result failed or not_allowed | Yes | `refused` ("{0} said no") |
| `HC_OK`, then timeout (30 s, or a garage door's whole move - press, release and its travel time - if longer), offline (generation dropped), or dropped by a catalog refresh | Yes | `no_answer` ("{0} didn't answer"). The `hc_refresh` fix in U1 makes a dropped row's pending action fail as offline (home_control.c:173-178 is silent today). |
| `HC_ERR_BUSY` | No | Fail at once (`busy`): a person is acting on that device |
| `HC_ERR_OFFLINE` or `HC_ERR_UNKNOWN` on the HA source | No | Held, tried every 2 s for up to 30 s. A held arming-state occurrence is dropped if the partition's state has changed since. After 30 s it is `missing` only if the ref is still absent **and** the source has been up 30 s or more (the catalog had time; catalog_begin empties the list before re-adds, device_service.c:147-167); otherwise `unreachable`. |
| `HC_ERR_OFFLINE` or `HC_ERR_UNKNOWN` on any source other than HA (Z-Wave in B) | No | Fail at once (`unreachable`, or `missing` when unknown). Phase B: `received_ms` is the moment device_service is asked to act, not the trigger's time: the runner holds some sends on purpose (the 10 s device gap, the cap, "Turn it off after"), which the trigger's time would make ZWO_STALE_MS (5 s, zw_outputs.h) refuse; a command that waits too long in the Z-Wave task's queue is still refused. The answer comes when the output layer takes or refuses the command, not when the device confirms. |
| `HC_ERR_NOT_ALLOWED` | No | `not_allowed`, a canary (should never happen): for a Lock or Close it goes to the failed port (`ALARM_EV_AUTOMATION_FAILED` reason not_allowed); other actions only set the status |
| `HC_ERR_ARG` | No | The action doesn't fit the device's live kind (a light's ref that is now a lock): the "can't run" status, as layer 4 |

- **Request ids.** `hc_act_auto` returns the request id, and `hc_port_t.result` gains it (U1), so results match runs.
- **Two actions** run independently, in order.
- **A new occurrence** replaces an older held send of the same automation (latest wins), so no backlog builds up.
- **An edit** (same uid) that changes the trigger's kind, zone or partitions clears its close hold and cooldown. One that changes an action (source, ref, kind, action, value or mode) drops that action's waiting send; an off timer already running for the old device keeps its own copy and still turns it off.
- **"Turn it off after"** starts its RAM timer when the On is accepted (`HC_OK`) and sends `ACT_OFF` when it expires. A new occurrence restarts the timer of the action's current device, with the action's current length, while the action still has a "Turn it off after"; an On without one sent (`HC_OK`) to that device stops it (the newest rule says "stay on"). An edit that changes only the length keeps a waiting On, which is sent once and starts its timer with the new length. Deleting the automation cancels the timer and an Off that is due but not sent yet. Turning the automation off lets a running timer finish. A restart loses the timer and the light stays on (documented).
- **The Off is a send of its own**, never overwritten by a new run. It waits for the device gap and for the cap (never `held_back`: an Off is never lost, unless its automation is deleted) and goes unless a newer On to the same device was sent (`HC_OK`) after the Off fell due; that On's timer then owns the device. An On that would start a timer while the action's timer still holds another device (after an edit) first makes that device's Off due at once, and waits until it has gone, at most 30 s from its first wait, like a held send; then it ends `held_back` if the cap is full, else unreachable.

**Loops and cascades (AUT-007):**
- **By construction.** Device actions emit no engine events, and an ARMED or DISARMED whose source is automation starts device actions only, never another arm or disarm (D23). The longest chain is trigger → ARM or DISARM → ARMED or DISARMED → device actions, one hop by design ("bedtime Night, then lock and lights off"; "10 AM disarm, then unlock"). A host test enumerates every trigger × action pair.
- **One partition.** At least 10 s (`AUTO_PART_GAP_MS`) between two automatic arms or disarms of the same partition, whatever started them.
- **Physical loops**, e.g. a light tripping a sensor: at least 10 s (`AUTO_DEVICE_GAP_MS`) between two automation actions on the same device. That is at most 0.1 Hz, far below WCAG 2.3.1, so house lights can never be made to strobe.
- **Panel-wide cap.** At most 12 actions in 60 s; the rest end as `held_back` ("Held back: too many ran at once").
- **One device twice** in one automation is refused at save.

**Arming (phase C):**
1. **Who.** A system master or installer picks Arm after any trigger (D23; first a time of day
   only); there is no installer switch (decided 2026-10-05: "the master decides"). The person who saves it is stored as
   `made_by`, and the automation arms as that person.
2. **Warning.** N minutes before (1-15, default 5, SCH-002):
   - the status bar shows a steady banner, and Home shows the card (section 9);
   - the existing chime plays once a minute (`sounder_play(ALARM_SOUND_CHIME)`, sounder.h:39; posted through `alarm_service_post` if it isn't safe off the alarm task) - and only when nothing else sounds: it never plays over, interrupts or delays an alarm, fire, CO, entry, exit or trouble sound (D22; C4 proves it with the sounder's mix test);
   - the arm is due at warning start + N minutes **on the monotonic clock**, so clock jumps can't shorten the warning.
3. **Never without a whole warning.** If the time is crossed without a whole warning (the clock was set at 22:28 for 22:30, or a restart lost it), there is no arm that day. The status says so and `automation_failed` reason `no_warning` is logged. A restart during the warning leaves nothing to report (risk R5).
   Edges, each failing safe (no arm, said in the status): a rule saved or turned on inside its own warning minutes, or an edit of its arming during the warning, reads "no warning" that day; an arm at 2:30 AM on the spring-forward night doesn't arm (one reading crosses both minutes); a warning that starts late (a small clock step, T2) still runs whole, so it arms that much after the time. Someone arming the partition during the warning ends it at once ("already armed"), and after an hour's put-off the warning comes back for a whole 5 minutes however late the tick.
4. **Put-off** (SCH-003, D26, D27). On every warning, one **Put off** button opens a choice, then the code pad: **An hour later** (the planned time + 60 minutes; the warning starts again 5 minutes before) or **Not tonight** (this automation doesn't arm before 6:00 AM; for an arm time between 6 AM and 6 PM it reads **Not today** and holds until midnight). While it waits, Home and the Overview show one steady line beside the partition ("Arms Night at 11:30 PM · put off by Pat" / "Won't arm tonight · put off by Pat"), hidden while that partition is armed, gone when the new warning starts or the hold ends; a restart loses it with the arming (R5). One warning at a time: while an automation warns or is put off its new triggers are ignored, and turning it off or deleting it ends the warning and the line at once. Each put-off emits `ALARM_EV_ARMING_PUT_OFF` (section 7). **Not tonight** needs a code that may disarm that partition (D31; the check answers that too); another code gets only the hour. The code goes through a new `ALARM_CMD_CODE_CHECK`:
   - it authenticates like any keypad code, so lockout counts;
   - it changes nothing;
   - for a duress code it emits the silent `ALARM_EV_DURESS` "as a disarm by that code", following the walk-test precedent (arming.c:673-675).

   There is no "Arm now": that would be quick arm, an installer option (14:100).
5. **At arm time.** The service calls `alarm_service_command_as(&{ARM, p, level, 0, ""}, &{AUTOMATION, uid}, made_by, ALARM_SVC_WHEN_DISARMED, &res)`.
   - The state check runs on the alarm task in the same call and also refuses installer mode (alarm_service.c:795-819).
   - If the partition isn't disarmed, the answer is BAD_STATE **without** an engine log, recorded only as "already armed". It never re-arms at another level, which would restart exit time and could turn a family's Night into Away (arming.c:136-139).
   - The engine re-checks that `made_by` still holds a user with that partition who may arm (arming.c:706-711, `can_arm`). A deleted or demoted maker stops the rule.
6. **Not ready.** Without "Bypass open zones" the arm fails. The engine logs `ARM_FAILED` and plays its error sound, and U-C2 makes `ARM_FAILED` REPORT_NOW for the AUTOMATION and SCHEDULE sources (arming.c:115-120 is REPORT_NONE today). During the warning the card has already said the panel is not ready (D28). Walk test, sounding and HELD are refused the same way.
   With it (D29), the command carries `ALARM_ARM_BYPASS_FAULTED`, narrowed for the automation source: the engine bypasses only burglary zones whose reason is an open (faulted) loop - a zone in tamper, loop or missing-sensor trouble still fails the arm, naming it, since `eng_zone_not_ready` counts those too (zones.c:8-26) and a tampered sensor can be an attack. One BYPASS event per zone with the automation as source (reported for reportable zones), then ARMED with an arg2 mark "bypassed at arming" ("Armed Night · By automation · 1 zone bypassed"). Fire, CO and 24-hour zones never block an arm and are never bypassed (zones.c:28-37). A bypass the automation made is marked; once that zone has been closed for 10 s it is unbypassed (UNBYPASS, source automation: "Kitchen Window watched again"), so a garage or window closed after arming is protected (as a returnable force arm does on classic panels; a bypass that lasts until disarm is the trap). Every bypass still ends at the disarm (arming.c:256-262). That path skips `can_bypass` today (arming.c:145-168 against 794-808), so the automation guard (C2) checks `can_bypass` for `made_by`; the maker is a system master or installer, who may bypass anyway.
7. **18's semantics.** Exit delay applies. There is no auto-stay (it converts keypad arming only, arming.c:372-373).

**Disarming (phase C, D23):**
1. **When.** At the trigger, with no warning (a disarm makes nothing sound). The service calls `alarm_service_command_as(&{DISARM, p, 0, ""}, &{AUTOMATION, uid}, made_by, ...)`.
2. **The engine** re-checks that `made_by` still holds a user with that partition who may disarm (`can_disarm`), that `opt.auto_less_secure` is on, and that the partition's alarm isn't sounding (D22). Each refusal is NOT_AUTHORIZED with the error sound; the status says why.
3. **Already disarmed** is only noted in the status. A refused disarm raises no event: it leaves the house as it was.
4. **Never undoes its own trigger.** A save is refused when an armed trigger (that partition, or any level) starts a Disarm of the same partition: it could only undo the arming (other systems block it; D24 asks no confirm for an armed trigger). The editor says so beside the action ("It would undo the arming at once"). Proposed (D31): the plain confirm also for an armed trigger with unlock or open ("Every time Home is armed Away, Front Door Lock will unlock").
5. **A nudge, not a rule.** Under a time-of-day Disarm the editor says "Nothing arms it again. Add one that arms in the evening." when no automation arms that partition (other systems pair a disarm schedule with an arming one, or re-arm if nobody comes). Holidays wait for exception dates (SCH-001).

## 4. Relationship to schedules (docs/features/18), outputs (16 OUT-005), Z-Wave and Zigbee device sources, notifications (26)

- **Feature 18.** "At 10:30 PM → Arm Night" *is* auto-arm. Phase C delivers it once through the AUTOMATION source.
  - Covered here: SCH-002 (warning, chime, screen), SCH-003 (put-off once by any code, +60 min), SCH-004 (`ARM_FAILED` reported, replacing 18's planned `auto_arm_failed` type, or the open zones bypassed when the automation says so, D29), SCH-005 semantics, SCH-008.
  - 18 keeps SCH-001 exception dates (a later trigger field), SCH-006 auto-disarm (for now; since D23 phase C's Disarm can deliver it the same way) and SCH-007 scheduled users.
  - `auto_clock` and `auto_arm` live in `components/core/automation`. They are extracted for `alarm_sched` only when 18 is actually built.
  - 18's design and status lines point to 41. "Time-driven outputs" in 18's goal moves to 41's later list.
- **Feature 16 OUT-005.** One table, not three. When BL-037 outputs exist, a relay becomes `AUTO_ACT_OUTPUT = 3`, On/Off only, and OUT-005's zone lists use a reserved field. The 16 shared rules meet "at least 8". 16's "small portable rule table" line points at 41.
- **Z-Wave (phase B).**
  - `DSRC_ZWAVE = 3` is appended to `dsrc_kind_t` (device_source.h:23-27), and `HC_MAX_SOURCES` goes from 2 to 3.
  - `dsrc_zw` sits over `zwave_service_ha_devices` with refs `zw_<home>_<node>_<gen>_<ep>` (proto_zw.h:36). Kinds map SWITCH → DEV_SWITCH, dimmer → DEV_LIGHT, GARAGE → DEV_COVER (close only).
  - The source **posts** its command and reports the result later. `zwave_service_ha_command` blocks up to 2 s (zwave_service.c:52) and would otherwise hold device_service's mutex (device_service.c:229-231) and stall the LVGL task.
  - Z-Wave registers before HA, so `HC_MAX_DEVICES` 64 can't crowd it out (hc_refresh fills in registration order, home_control.c:181-201).
  - Z-Wave outputs then also appear on Smart Home (24:129-132 "leaning yes"), which needs its own small mockup.
  - `CONFIG_ALARM_HOME_CONTROL` is relaxed to `SPIRAM && (ALARM_FEATURE_HA || ALARM_ZSRC_ZWAVE)` (device_service/Kconfig:6).
- **Zigbee.** When BL-036/BL-149 actuators get a command path, `DSRC_ZIGBEE` joins the picker with no change here: `hc_act_auto` gates every source.
- **Notifications (26):**
  - `automation_failed` is in TROUBLE, on by default (NOTE-037). It is raised only for Lock and Close failures and C's `no_warning`. A porch light that failed shows only on its automation.
  - An ARMED by an automation reads "By automation" in ARMING, as any arming does.
  - `ARM_FAILED` from an automation is reported in ARMING with the existing "Arming failed" words plus the zone.
  - The warning and put-off push nothing to phones (the ARMED or "Arming failed" that follows says enough).
  - Silent rules (NOTE-003/038) are untouched: automations never react to silent events.

## 5. Architecture

**Components (ADR-034 tiers):**

| Tier | Component | Holds |
|---|---|---|
| core (new) | `components/core/automation` | `automation.h` (model, limits, validator), `auto_codec.c`, `auto_model.c`, `auto_watch.c` (observer), `auto_clock.c` (time crossing), `auto_run.c` (runner), `auto_arm.c` (C). Portable C11: no heap, no globals. Depends on alarm_core, home_control, rtc_clock. |
| core | `home_control` | `hc_auto_allowed`, `hc_act_auto`, `HC_ERR_NOT_ALLOWED`, the request id in results, the dropped-row fix; `dsrc_zw` (B) |
| core | `alarm_core` | `ALARM_SRC_AUTOMATION = 5` ("automation"), the dispatch guard, `ALARM_EV_AUTOMATION_FAILED`; in C: `ALARM_CMD_CODE_CHECK`, ARM_FAILED reporting |
| core | `notify`, `alarm_proto` | Words, "By automation", JSON keys |
| services (new) | `components/services/automation_service` | NVS store, "auto" task, listener, ports, console `automations` (read-only list and status) |
| services | `device_service`, `platform_esp`, `panel_web` (`web_automations.c`), `ui_port_esp`, `zwave_service` (B) | Act and result hooks; `MAX_LISTENERS` 12; web API; UI hooks; posted Z-Wave output |
| ui | `alarm_ui/src/ui_automations.c` | Screens |

**The portable core:**

```c
/* automation/automation.h - stored enums are append-only */
#define AUTO_MAX 16u
#define AUTO_ACTIONS 2u
#define AUTO_KEEP_MAX 64u                       /* ignorable unknown TLV bytes kept per record */
typedef enum { AUTO_TRIG_NONE = 0, AUTO_TRIG_ARMED = 1, AUTO_TRIG_DISARMED = 2, AUTO_TRIG_ALARM = 3,
               AUTO_TRIG_FIRE = 4, AUTO_TRIG_ZONE_OPENED = 5, AUTO_TRIG_ZONE_CLOSED = 6,
               AUTO_TRIG_TIME = 7 } auto_trig_t;
typedef enum { AUTO_ACT_NONE = 0, AUTO_ACT_DEVICE = 1, AUTO_ACT_ARM = 2 /* 3 OUTPUT reserved */,
               AUTO_ACT_DISARM = 4 } auto_act_kind_t;
typedef enum { AUTO_REC_OK = 0, AUTO_REC_NEWER, AUTO_REC_BROKEN, AUTO_REC_BAD } auto_rec_t;
typedef struct { uint8_t kind, partitions, levels, days; uint16_t zone, minute, win_from, win_to; } auto_trigger_t;
typedef struct { uint8_t kind, source, dev_kind, dev_action, confirmed, arm_partition, arm_level, warn_min;
                 int16_t value; uint16_t off_after_min;
                 char mode[DEV_MODE_LEN], ref[DEV_REF_LEN], name[DEV_NAME_LEN]; } auto_action_t;
typedef struct { uint16_t uid; uint8_t slot, made_by, n, rec; bool off;
                 auto_trigger_t when; auto_action_t then[AUTO_ACTIONS];
                 uint8_t keep_len, keep[AUTO_KEEP_MAX]; } auto_rule_t;
typedef struct { uint8_t type, partition, dev_class; } auto_zone_t;          /* from alarm_config_t */
typedef struct { uint8_t partitions_enabled; bool arm_allowed, arm_supported;
                 const auto_zone_t *zones; /* [ALARM_MAX_ZONES] */ } auto_env_t;
typedef enum { AUTO_OK = 0, AUTO_ERR_INCOMPLETE, AUTO_ERR_NOT_ALLOWED, AUTO_ERR_ZONE, AUTO_ERR_PARTITION,
               AUTO_ERR_TIME, AUTO_ERR_WINDOW, AUTO_ERR_ARM_ALONE, AUTO_ERR_MAKER,
               AUTO_ERR_NEEDS_CONFIRM, AUTO_ERR_LESS_SECURE_OFF, AUTO_ERR_SAME_DEVICE, AUTO_ERR_UNSUPPORTED } auto_check_t;
auto_check_t auto_rule_check(const auto_rule_t *r, const auto_env_t *env);
bool   auto_zone_usable(uint8_t zone_type, uint8_t dev_class, bool for_close);
size_t auto_encode(const auto_rule_t *r, uint8_t *out, size_t cap);
auto_rec_t auto_decode(const uint8_t *in, size_t len, auto_rule_t *out);
size_t auto_nvs_entries(size_t record_bytes);

/* auto_watch.h - alarm task, never blocks */
typedef struct { uint8_t kind, partition, level, cause; uint16_t zone; alarm_ms_t at; } auto_occ_t;
void   auto_watch_init(auto_watch_t *w, alarm_ms_t now, uint32_t settle_ms);
size_t auto_watch_event(auto_watch_t *w, const alarm_event_t *ev, auto_occ_t *out, size_t max);
size_t auto_watch_snapshot(auto_watch_t *w, const alarm_snapshot_t *s, auto_occ_t *out, size_t max);
void   auto_watch_zones_reset(auto_watch_t *w, const uint8_t *zone_bits);

/* auto_clock.h */
typedef struct { bool valid; int64_t epoch_s; int32_t day_no; uint16_t minute; uint8_t weekday; } auto_local_t;
typedef struct { int32_t day_no; uint8_t weekday; uint16_t from, to; } auto_span_t;   /* inclusive [from, to] */
size_t auto_clock_step(auto_clock_t *c, uint64_t mono_ms, const auto_local_t *lt, auto_span_t out[2]);
uint32_t auto_clock_gen(const auto_clock_t *c);
int32_t  auto_clock_floor(const auto_clock_t *c);
bool   auto_time_due(const auto_trigger_t *t, const auto_span_t *s, int32_t *last_run);  /* AUTO_RUN_NONE = never */
/* After each auto_clock_step, if auto_clock_gen changed, set every remembered last_run greater than
 * auto_clock_floor to AUTO_RUN_NONE before calling auto_time_due on that step's spans. */

/* auto_run.h - automation task */
typedef struct {
    void *ctx;
    int  (*device_act)(void *ctx, const auto_action_t *a, uint32_t *request_id);   /* hc_result_t */
    bool (*source_settled)(void *ctx, uint8_t source);       /* up and listed >= 30 s */
    bool (*device_kind)(void *ctx, uint8_t source, const char *ref, uint8_t *kind); /* live kind; NULL: can't run */
    alarm_result_t (*arm)(void *ctx, uint16_t uid, uint8_t p, uint8_t level, uint8_t maker,
                          bool *already, uint16_t *zone);    /* phase C */
    void (*warning)(void *ctx, const auto_warning_t *w);     /* phase C: mailbox, never a UI lock */
    void (*failed)(void *ctx, uint16_t uid, uint8_t reason, uint8_t dev_action, uint16_t zone);
} auto_ports_t;
void     auto_run_init(auto_run_t *r, const auto_ports_t *p, uint64_t now_ms);
void     auto_run_rules(auto_run_t *r, const auto_rule_t *rules, size_t n, const auto_env_t *env);
void     auto_run_occurrence(auto_run_t *r, const auto_occ_t *o, const auto_local_t *lt, uint64_t now_ms);
void     auto_run_time(auto_run_t *r, const auto_clock_t *c, const auto_local_t *lt,
                       const auto_span_t *s, size_t n, uint64_t now_ms);
void     auto_run_result(auto_run_t *r, uint32_t request_id, bool ok, uint8_t why, uint64_t now_ms);
void     auto_run_tick(auto_run_t *r, uint64_t now_ms);
uint64_t auto_run_next_deadline(const auto_run_t *r, uint64_t now_ms);
auto_try_t auto_run_try(auto_run_t *r, uint16_t uid, uint64_t now_ms);   /* device actions only */
bool     auto_run_put_off(auto_run_t *r, uint8_t partition, uint8_t user, uint64_t now_ms); /* C */
void     auto_run_status(const auto_run_t *r, uint16_t uid, auto_status_t *out);
/* auto_res_t (with NO_CLOCK), auto_status_t, auto_why_t and auto_try_t: as in auto_run.h. */
```

**Service and threading:**
- **Listener.** `on_event`, `on_snapshot` and `on_zones_reset` run `auto_watch_*` on the alarm task. The state is about 64 bytes of internal static RAM, touched only there. Occurrences go into a 32-deep queue with a 0 timeout, then `xTaskNotifyGive`, as in zwave_service.c:148-173. `clock_ok` comes from `sys_troubles` in the same snapshot.
- **The "auto" task.** Priority 4, a 6 KB **PSRAM** stack. It never writes NVS (internal RAM is the reserve, B001). It blocks on notify until `min(next_deadline, 1 s while any time rule or warning exists, else 60 s)`. Each wake:
  1. drain occurrences;
  2. drain device results;
  3. the rules again after an edit, and the zones (`auto_zone_t[128]`) after a `config_seq` change, through `alarm_service_get_config` into a PSRAM buffer allocated once; then a waiting Try it now;
  4. read local time and step the clock (`auto_run_time`);
  5. `auto_run_tick`.

  It calls `device_service_act_auto` and, in C, `alarm_service_command_as`. It never holds `s_auto_lock` while calling a service (it copies the rule out first).
- **Device results.** `device_service_set_result_listener(fn, ctx)` is called from `port_result` (device_service.c:58-64) under the device lock and only enqueues.
- **Saves** run on the caller's task: the LVGL task (holding `board_ui_lock`) or the HTTPS worker, both on internal stacks. A save mutex serialises them. Each save validates, encodes, writes NVS, then swaps the rule in under `s_auto_lock` and notifies the task. A store `gen` (u16, in `meta`) makes a stale browser page get `changed`.
- **Warning (C).** The task writes a small mailbox (partition, level, arm time, put-off state) under `s_auto_lock`. The UI reads it from its 1 s tick and the web from GET. The auto task never takes `board_ui_lock`.
- **Listener slots.** `MAX_LISTENERS` goes from 8 to 12 (alarm_service.c), with a `_Static_assert`; the automation listener is the ninth. panel_app wraps the other registrations in `ESP_ERROR_CHECK`, and the CrowPanel used all 8. panel_app registers `automation_service_listener()` beside them, before `alarm_service_start`, **without** `ESP_ERROR_CHECK`, and tells the service whether it was taken (`automation_service_listener_added`): if not, automations are off with a log line (no task; Try it now answers busy) and the alarm runs on. A start-up log prints the slots used.
- **Settle.** The configuration can't be read before the alarm starts, so the observer starts with a 60 s settle; when the configured `power_up_settle_s` differs, `automation_service_start` re-initialises it on the alarm task and the next snapshot is its new baseline.
- **Local-time port.** `gettimeofday` + `localtime_r` with the panel's POSIX TZ (net_wifi.c:136-142), as zwave_service.c:126-146 does, plus the weekday and `day_no`.

**Engine changes** (each with a CORE row in docs/features/01 and a tagged test):
- A: append `ALARM_SRC_AUTOMATION = 5` (alarm_types.h:312-318) and name it "automation" (strings.c:253-263); the dispatch guard (refuse all); `by_whom` tests AUTOMATION **before** the user (notify_words.c:222-255), so an arm run as Alex reads "By automation" while the JSON keeps `user_name`; `ALARM_EV_AUTOMATION_FAILED`.
- C: the guard relaxed as in section 3; `ALARM_CMD_CODE_CHECK` appended to `alarm_cmd_type_t` (alarm_types.h:336-345); `arm_failed()` REPORT_NOW for AUTOMATION and SCHEDULE, with the zone carried to the words.
- `quick_allowed` is unchanged: automations arm only `as` the maker.

**Kconfig.** `CONFIG_ALARM_FEATURE_AUTOMATIONS` goes in alarm_features and `depends on ALARM_HOME_CONTROL` (HCTL-010).
- It is stated in both boards' sdkconfig.defaults: P4 `y`, e32r40t `n`. Run `reconfigure` after adding it.
- When off: no service, no listener, NULL `ui_backend_t` hooks, no Settings line, no route, no `capabilities.automations`.
- `ALARM_UI_WITH_AUTOMATIONS` follows it, and `host_features.h` defaults it to 1.

**Simulator.** `host/ui_preview/host_panel.c` links the real core:
- `on_event` (343-358) and each post-call snapshot feed `auto_watch`. Occurrences are queued and processed **after** the engine call returns, never inside a port.
- `host_panel_advance` (2767-2797) adds `auto_run_next_deadline` to its deadline ordering and ticks with the host clock under the fixed TZ `EST5EDT,M3.2.0,M11.1.0`.
- The device port acts on the fake `ui_dev_t` list (4763-4792), answers after 300 ms, and can mark a device offline or missing, so a rule visibly changes a Smart Home tile.
- The arm port calls `alarm_engine_command_as`. Harbor House automations are seeded.

## 6. Stored format

**Namespace and keys.**
- Namespace `automations` (11 characters, within BACKUP_NAME_MAX 15).
- `meta` = compat u8 (1) | gen u16 LE | next_uid u16 LE.
- `a01`..`a16`, one record per key, so an edit rewrites one record (the zbpanel convention).

**Record.**
- The first byte is compat (1; 0xFF means erased flash; 0 is no record either: both load as BAD). Then TLV `id u8 | len u8 | value`, little-endian, bytes cast to `unsigned` before shifting (CI GCC 13).
- Zero fields are not written, except where marked.
- **Ids below 0x80 are critical.** An unknown one, or an unknown value in a known enum field (a trigger kind of 9), loads as `AUTO_REC_NEWER`: shown, read-only, never run. An older firmware never runs a rule whose meaning it doesn't know.
- **Ids 0x80 and above are ignorable.** They are kept, up to 64 bytes, and written back. More than 64 bytes makes the record read-only.
- **A higher compat** is `NEWER` for that record only. The rest of the store stays editable.
- A phase-A build decodes every field below but reports Arm actions as `AUTO_ERR_UNSUPPORTED`, i.e. NEWER. No format change happens between phases.

| Id | Field | Size |
|---|---|---|
| 0x01 | uid (stable, never reused; the event source id) | u16 |
| 0x02 | flags: bit0 off (absent = on) | u8 |
| 0x04 | made_by user (written for Arm rules) | u8 |
| 0x10 | trigger kind (`auto_trig_t`) | u8 |
| 0x11 | partition mask (0 = any) | u8 |
| 0x12 | level mask (`ALARM_LEVEL_BIT`; 0 = any) | u8 |
| 0x13 | zone 1..128 | u16 |
| 0x14 | minute of day 0..1439 (absent = 00:00) | u16 |
| 0x15 | days, bit0 Mon..bit6 Sun (0 = every day) | u8 |
| 0x16 / 0x17 | window from / to, minute+1 (absent = any time) | u16 / u16 |
| 0x18 | reserved: zone bitmap (OUT-005 zone lists) | 16 |
| 0x20+n / 0x30+n | action 1 / action 2, sub-ids n: 0 kind, 1 source (`dsrc_kind_t`), 2 dev_kind it was made for, 3 dev_action, 4 value i16 (**absent = −1**, written whenever present, 0 included), 5 mode word ≤23, 6 ref 1-95 bytes no NUL, 7 name snapshot ≤47, 8 off_after minutes u16, 9 confirmed u8, A arm partition, B arm level, C warning minutes | — |
| 0x80 | reserved (ignorable): typed name ≤47 | — |
| 0x82 | ignorable: bit n = action n+1's "Bypass open zones" (D29), on an Arm only; an older firmware arms without bypassing (fails on an open zone) | u8 |
| 0x83 | ignorable: `changed_at`, epoch seconds when the sentence last changed (D30); an older firmware shows no "changed since" and keeps the old value when it edits | u32 |

**Limits and cost.**
- 16 records, 2 actions each, refs of 95 bytes or fewer.
- A typical porch-light record is about 75 B, which is 5 entries by `1 + ⌈b/4000⌉ + ⌈b/32⌉`. The worst case (`AUTO_RECORD_MAX`: two actions with every field full, plus 64 ignorable bytes, and phase C's `bypass_open` and `changed_at`) is 518 B, 19 entries.
- The store as a whole: about 80 entries typical, **307 worst case** with meta (308 with the namespace's own), out of 3,906 usable.
- `AUTO_MAX * auto_nvs_entries(AUTO_RECORD_MAX)` plus meta is pinned at 307 (and held at most 310) by `_Static_assert`s (test_auto_codec.c), as test_zbz_contract.c:110 does.
- The record has no end marker: a record cut exactly between two fields would decode as a shorter rule. NVS's own
  checksums are what protect against that in practice (a blob is written and checked whole).
- An add is refused below a reserve modelled on `ZBZ_NVS_ADD_MIN` (zbz_panel.h:145), measured with `nvs_get_stats` (zwave_service.c:300-321), with the words "There's no room left to store it."
- RAM: about 6.5 KB of rules in `EXT_RAM_BSS_ATTR`.

**Load outcomes** follow alarm_store.c:105-189:
- an absent key is an empty slot;
- an undecodable record is kept, shown "can't run", and never overwritten;
- any other read error makes the store read-only, and edits are refused.

**Wear.** Writes happen only on edits. Last-run status is RAM-only.

**Backup.**
- `automations` is added to `backup_policy.c` RULES and NAMES (38-60), with a `_Static_assert` and the reason "household choices, not identity". `test_policy_keeps_the_installation_not_the_identity` (test_backup.c:478-509) pins `automations/meta` and `automations/a01`.
- After a restore, missing devices show as missing until their source lists them. Refs are never pruned while the registry is empty (the ui_dev_model.h:238-243 precedent).

**OTA rollback.**
- A firmware without automations never opens the namespace.
- An older automation firmware keeps newer records read-only, or keeps their ignorable bytes.
- Phase C adds no configuration field: arming needs only the record and its maker, so a
  rollback keeps arm rules as NEWER (read-only) in a phase-A firmware.

**Fixture.** `components/core/automation/test/auto_fixture_v1.h`, hand-written and never edited:
- (a) the porch light: zone 1 opens, 18:00-06:00, Turn on `light.porch` (value absent) for 10 minutes;
- (b) armed Away → Lock `lock.front_door` · Run `scene.good_night` (confirmed);
- (c) 23:00 Mon-Fri → Turn off `climate.hallway`;
- (d) 22:30 → Arm Night on partition 1, warning 5, made_by 2;
- (e) a critical unknown id 0x7E → NEWER;
- (f) an ignorable 0x81 that must survive an `off` toggle byte for byte;
- (g) compat 2 → NEWER;
- (h) `ACT_RUN` on a script → BROKEN;
- (i) `ACT_UNLOCK` on a lock → OK (D23; the installer option never makes a record BROKEN);
- (j) a Disarm of partition 1, made_by 2 → OK (decoded; phase C runs it).

**Docs.** The docs/features/14 storage table and "What it holds"; docs/features/20 devices.md's NVS budget.

## 7. Events and logging

- **No run is logged.** A porch light would add dozens of records a day to a ring of at least 4,096 (LOG-002), and HCTL-007 doesn't log light changes.
- **Last run** is a per-automation RAM status: `auto_status_t {result, at_epoch_ms, action_index, reason, user}` (user = who put it off). After a restart it reads "hasn't run since the panel started". A held send for an ARMED or DISARMED occurrence that a newer change of that partition drops was never sent, so it leaves "Last run" as it was.
- **Arming or disarming by an automation** is the engine's own `ARMED` or `DISARMED`, with source {automation, uid} and user = the maker (D30: never read as the maker's).
  - Activity: the level words, then "By automation · " and the automation's trigger clause ("Disarmed · By automation · At 10:00 AM on weekdays"), cut to fit; the whole sentence and "Set up by Alex" where there is room (the Panel Manager's Activity, the automation's page). `notify` gains a hook from uid to sentence; "By a deleted automation" when the uid is gone. The record gains `changed_at` (an ignorable field, epoch seconds, set when its sentence changes), so a line older than the edit reads "By automation · changed since".
  - Everywhere else that names who armed: the engine keeps the arm source (alarm_engine.h:100, arming.c:177-182) but the snapshot exposes only `armed_by`, so the snapshot gains `arm_source` (engine.c:521). Home's armed card ("Since 10:30 PM · By automation", ui_app.c:1462-1469, 1636-1640), MQTT `armed_by` (messages.c:224-230) and the Panel Manager's `armed_by_name` (panel_web.c:1032-1033) read it first. It isn't persisted as `armed_by` is (engine.c:237, 678): after a restart they fall back to the last ARMED event's source, or the stored runtime state gains it (a contract change, decided when built).
  - MQTT and the HA link carry `"source":"automation"`, `"source_id":uid`, plus `automation` (its trigger clause, "At 10:00 AM on weekdays"; null when the panel can't
    say) and `saved_by`; `user`/`user_name` stay for compatibility, and consumers read the source first. This is an additive contract; the ha-addon sets `changed_by` to "Automation: At 10:00 AM on weekdays".
- **Put-off** (D26, D30): appended `ALARM_EV_ARMING_PUT_OFF` (`arming_put_off`, ARMING category, reported): user = who, source {automation, uid}, partition, arg 1 = an hour, 2 = the night (or day). Activity: "Arming put off an hour · By Pat · until 11:30 PM" / "Night arming skipped tonight · By Pat". MQTT adds `choice` and `until`; only a skip pushes a phone notification. (Contact ID 464 "auto-arm time extended" and SIA CE are the reference meanings; we send neither.)
- **New `ALARM_EV_AUTOMATION_FAILED`** (`automation_failed`), the **next free value at commit** (63 today; BL-045's unlock and open events may land first):
  - a platform event via `alarm_service_platform_event(type, zone, arg, arg2, src, user)`, with a `platform_event_ok` case (program.c:155-198, partition 0, REPORT_NOW);
  - `src` = {AUTOMATION, uid}; `arg` = reason (append-only: 1 unreachable, 2 no_answer, 3 refused, 4 missing, 5 busy, 6 not_allowed, 7 no_warning); `arg2` = `dev_action_t` (0 for arming); `zone` = the triggering zone or 0;
  - emitted only for Lock and Close failures, and C's `no_warning`;
  - deduplicated: at most one per automation per hour unless the reason changes.

  Words (notify_words.c, NOTE-001, TROUBLE category):

  | Case | Title | Line |
  |---|---|---|
  | Lock | "An automation didn't lock" | "{0} didn't answer" / "{0} wasn't reachable" / "{0} said no" / "{0} isn't on the panel now" / "{0} was busy". {0} is the zone or else "The device", because MQTT and notify see only `alarm_config_t` (messages.c:442-502), so no device name. |
  | Close | "An automation didn't close" | same lines |
  | Arm (C) | "An automation didn't arm" | "It had no time to warn first" |

  JSON keys: `automation` (uid), `action` ("lock" / "close" / "arm"), `reason` (word), with golden payloads in test_proto.c. The discovery `event_types` document grows about 20 bytes (about 1,034 used, limit 2,048, test_proto.c:707).
- **ARM_FAILED** becomes REPORT_NOW for the AUTOMATION and SCHEDULE sources. Its body gains the not-ready zone ("Arming failed · Kitchen Window · By automation"); today the body is only the result text (notify_words.c:400-405). This is ARMING category. The ha-addon already knows `arm_failed` (event.py:30), and 18's `auto_arm_failed` type is dropped.
- **No warning or change events in v1** (D14); the put-off is an event since D30. Its user also shows in the status ("Put off by Alex until 11:30 PM").
- **No automation entities in Home Assistant**: HA has its own automations.
- **ha-addon:** `EVENT_TYPES` gains `automation_failed`, plus tests (a separate-repository unit). It already lags 23 names and drops unknown ones (event.py:29-38, 72-73).

## 8. Access control

| Who | Touchscreen | Panel Manager |
|---|---|---|
| System master, installer, installer mode | Settings > Automations (`ACCESS_SYSTEM`, ui_settings.c:233-258): see, add, change, turn off, delete, Try it now | `GET automations`, `GET automations/devices`, `POST automations` at `ACC_SYSTEM`. Grant sessions are refused for POST (`403 password_sign_in_required`, panel_web.c:1783-1784). |
| Partition master, standard, arm-only, guest | The Settings line opens the locked page | Page hidden (`access:'system'`); GET answers 403 |
| Anyone at the wall (C) | Put off arming, with any user code (SCH-003). A duress code reports duress silently. | — |
| While armed or in alarm | Settings gear dimmed (UI-022). Automations keep running. | GET is read-only with a notice. POST answers 409 `locked` (panel_web.c:300-317), not in `locked_exempt`. `automation_service_save` also refuses when `alarm_snapshot_locked()`. |

- **Alarm and less secure actions** are made by a system master or installer, like every
  automation; only the less secure ones have an installer option (D25). They run as that person, so
  they are never more than that person could do at the keypad.
  - The engine checks the maker on every arm and disarm (`can_arm`, `can_disarm` for that partition);
    the runner checks before an unlock, open or stop that the maker is still a system master or installer.
  - Saving makes the editor the maker of every automation. The page shows "Arms as Alex" or "Disarms as Alex".
- **Server enforcement.** Every save runs `auto_rule_check`, so a forged POST can't skip the confirm, the option or the maker.
- **Console.** `automations` lists and shows status. It has no try and no edit, so there is no code-less arming from USB.
- **docs/features/14 "Who can change what"** gains one row:
  - Automations, an arming one included (it arms as the person who saved it) | system master or installer | Settings > Automations; Panel Manager > Automations | not while armed or in alarm
- **Escalation tests:**
  - a partition master's GET gets 403;
  - a deleted or demoted maker makes the engine answer NOT_AUTHORIZED. The engine logs nothing for an `as` refusal
    (only `eng_arm` logs ARM_FAILED, arming.c:118), so the service reports it itself (phase C chooses: a platform
    ARM_FAILED or `automation_failed` with an appended reason);
  - an AUTOMATION-source PANIC, SILENCE or BYPASS gets NOT_AUTHORIZED even with a code, and so does a DISARM while the option is off or that partition's alarm sounds.

## 9. Touchscreen

**Place.** Settings gets a tenth line, **Automations** (glyph `zap`), placed after Integrations.
- `SECTION_AUTOMATIONS = 9` (appended; it is the XML `ref_value`, ui_internal.h:112-122). It is shown through a `bind_flag` only with the feature.
- The column is full: 9 items use about 513 of 512 px (globals.xml:24-27). The proposal draws the list with finding 7 applied (Event log moved into Activity), as `replaces: 'settings-display'` (D2).
- It is not on the rail, which is full (rule 9).
- It is not on Smart Home, which vanishes with no device (HCTL-003), isn't locked while armed, and isn't needed by arm-only rules.

**Drawings:** built, so today's mockup (`docs/features/07-UI-LVGL-Pro/mockup/screens/automations.js`, with the Home warning in `home.js` and the Activity rows in `views.js`) shows them and the proposal files are deleted; the Panel Manager's pages are `panel-pages.js` itself. Phase B's Smart Home change (BL-169) isn't drawn.

Pages: `PAGE_AUTOMATIONS`, `PAGE_AUTOMATION` (arg = slot). Everything else is a sheet, so the stack stays at 3 of 6.

**Words used across screens.**

| Kind | Whole sentences (`TR`, `I18N_FORMAT`; names are values) |
|---|---|
| Arming triggers | "When the panel is armed" · "When the panel is armed Away" / "…Stay" / "…Night" · with more than one partition "When {0} is armed" / "When {0} is armed Away" / "…Stay" / "…Night" |
| Disarm triggers | "When the panel is disarmed" / "When {0} is disarmed" |
| Alarm triggers | "When an alarm sounds" · "When a fire or CO alarm sounds" |
| Zone triggers, by class | "When {0} opens" (door, window, garage) · "When {0} sees motion" · "When {0} detects water" · "When {0} is triggered" (others) · "When {0} closes" |
| Time triggers | "At {0} every day" · "At {0} on weekdays" · "At {0} on weekends" · "At {0} on {1}" ({1}: short day names joined by a translatable list separator) |
| Window | "Any time" · "{0} to {1}" |
| Actions | "Turn on {0}" · "Turn on {0} for {1}" ({1} through TRN "{0} minute"/"{0} minutes", "1 hour") · "Turn off {0}" · "Lock {0}" · "Close {0}" · "Run {0}" · C: "Arm Away" / "Arm Stay" / "Arm Night" · "Arm {0} Away" / "Arm {0} Stay" / "Arm {0} Night" |
| Reasons | "{0} didn't answer" · "{0} wasn't reachable" · "{0} said no" · "{0} isn't on the panel now" · "{0} was busy" · "Too many ran at once" |

- Two clauses are joined only by the house separator " · ". Times use `ui_minute_text`/`ui_clock_text`, days `ui_time_or_day_text`.
- Every word carries `/* TR: */` budgets. `ui_automations.c` joins `po/MARKED`, and the en-XA walk must show nothing cut off.

**Screens** (proposal ids; each lists title, rows, controls, states):

1. **`settings-automations`** (today's Settings list, `kit.js` SECTIONS). The Settings list:
   - Screen and sound, Users, Wi-Fi and network, Time and language, Integrations, **Automations** (zap), Walk test, About, a line, Installer.
   - Event log is gone because finding 7 moved it into Activity. A note on the screen says this depends on finding 7.
2. **`automations`**. Title "Automations". Note "{0} of 16 automations".
   - Row action (blue, plus): **"Add an automation"**.
   - One `row_link` per automation, in slot order:
     - chip glyph by the first action: lightbulb, toggle-right, fan, lock, door-closed, thermometer, sparkles; moon or shield for an arm. Grey when off.
     - **title** = the trigger sentence;
     - **detail** = the action sentence or sentences joined by " · ";
     - **value** = "Off" (grey) when off, empty when on and fine; chevron.
   - Problems use `row_link_alert` with an amber sub-line:
     - "Didn't work at 10:30 PM"
     - "Porch Light isn't on the panel now"
     - "Made by newer software"
     - "Can't run: open it to fix it"
   - Example rows (Harbor House):

     | Title | Detail | State |
     |---|---|---|
     | "When Front Door opens" | "Turn on Porch Light for 10 minutes" | on |
     | "When the panel is armed Away" | "Lock Front Door · Run Good Night" | amber "Didn't work at 8:02 AM" |
     | "When a fire or CO alarm sounds" | "Turn off Hallway" | on |
     | "When Basement Leak detects water" | "Close Water Main" | on |
     | "At 11:00 PM on weekdays" | "Close Garage Door" | value "Off" |
3. **`automations-empty`**. The Add row, then the paragraph: "The panel can do things by itself, like turning on the porch light when the front door opens."
4. **`automations-full`**. The Add row disabled, with detail "16 is the most. Delete one to add another."
5. **`automations-clock`**. The list with an amber paragraph above the rows: "The clock isn't set, so times wait until it is."
6. **`automations-locked`**. `ui_settings_locked` with the row **"Enter the master or installer code"**: the string `ACCESS_SYSTEM` produces (ui_settings.c:273-277). Note: "Automations are changed with the master or installer code."
7. **`automation-add`**. Pick sheet (fit) **"Add an automation"**. Each idea shows only when its parts exist. Nothing is created until the last pick (the "Add a zone" pattern), and Cancel at any step leaves nothing.

   | Row | Detail | Picks | Creates |
   |---|---|---|---|
   | "Turn on a light when a door opens" | "Like the porch light, in the evening" | a door or window, then a light | window 6:00 PM to 6:00 AM, 10 minutes |
   | "Lock up when you arm Away" | "Lock a door when everyone has left" | a lock | — |
   | "Turn on lights when an alarm sounds" | "So you can see, and be seen" | a light | — |
   | "Close the water when a leak is found" | "Shut the water main" | a water sensor, then a valve | — |
   | "Start from scratch" | "Choose when it runs and what it does" | — | — |

   Phase C adds **"Arm Night at bedtime"**, "Every night at a time you choose, after a warning", when the option is on.
8. **`automation-when`**. Pick sheet **"When should it run?"**:
   - "When the panel is armed" (shield) — "Once exit time is over"
   - "When the panel is disarmed" (house)
   - "When an alarm sounds" (siren) — "Never for a silent alarm"
   - "When a fire or CO alarm sounds" (flame)
   - "When a door or window opens" (door-open) — "Or a motion or water sensor"
   - "When a door or window closes" (door-closed)
   - "At a time of day" (clock) — "On the days you pick"
   - Note: "Silent alarms and duress never start an automation."
9. **`automation-armed-how`**. Pick sheet **"Armed how?"**: "Away", "Stay", "Night", "Any of them".
10. **`automation-partition`**. Pick sheet **"Which partition?"**: the partition names, then "Any of them". Shown only when more than one partition is enabled.
11. **`automation-zone`**. Pick sheet **"Which door or window?"** (or **"Which sensor?"** for opens).
    - Only usable zones, glyph by class, detail "Zone {0}".
    - Note: "Silent, fire and button zones aren't listed."
    - Empty: a row "No doors or windows to choose" (greyed).
12. **`automation-time`**. Sheet **"Run at"**:
    - One `time_rollers` set (Hour, Minute in 5-minute steps, AM/PM).
    - Below it seven 64 px day toggles in the region's week order, using the catalog's short day names (`day_toggle`; chips are only 40 px and the segment is 44 px).
    - Result line: "Runs every day at 11:00 PM" / "Runs on weekdays at 7:30 AM" / "Runs on weekends at 9:00 AM" / "Runs on {0} at {1}" / "Pick at least one day" (OK disabled).
    - Note: "It runs once a day at most, and waits while the clock isn't set."
    - Cancel / OK.
13. **`automation-then`**. Pick sheet **"What should it do?"**:
    - Devices grouped by area, as "Add to Favorites", glyph by kind. Detail = area, or "Not reachable now" (still pickable).
    - Devices with no allowed action are left out, and so are scripts.
    - Note, only while the installer option is off: "Your installer has turned off automations that unlock, open or disarm."
    - Empty: a row "No devices to choose", detail "Devices appear here once one is connected."
14. **`automation-device-do`**. Pick sheet **"What should {0} do?"**, with only allowed rows:
    - "Turn on" / "Turn off" (light, switch, fan); "Turn off" (thermostat). A lock, cover, valve or scene skips this sheet.
    - When editing, also "Choose another device", and "Remove this action" (red) when there are two.
15. **`automation-confirm`**. A non-danger confirm dialog:
    - Scene: title "Use Good Night in an automation?"; note "The panel can't see what a scene does. If it unlocks or opens anything, it will do so every time this runs."; buttons **"Use it"** / **"Cancel"**.
    - Switch turned on: title "Use Christmas Lights in an automation?"; note "Use it only for a light or a plug, never for a switch that opens a door, a gate or a garage."; buttons **"Use it"** / **"Cancel"**.
    - Asked once, when picked, and stored as `confirmed`.
16. **`automation`**. The automation's page. Title **"Automation"**.
    - Top `row_toggle` (zap) **"Use this automation"**. Its detail carries the state (rule 2), changing at most once a second (UI-023) through `ui_row_set_detail` from the 1 s tick:
      - "On · ran at 7:42 PM"
      - "On · ran on Sep 19"
      - "On · hasn't run since the panel started"
      - "On · waits until the clock is set"
      - "On · tried at 9:44 PM"
      - "Off · it won't run"
    - Section **"When"**: a `row_link` with the trigger glyph, title = trigger sentence, detail "Zone 1" / "Once exit time is over" / "Never for a silent alarm". It opens `automation-when`.
    - Then **"Only between"** (clock), value "Any time" or "6:00 PM to 6:00 AM", opening `automation-window`. Hidden for time triggers.
    - Section **"Then"**:
      - one `row_link` per action: the action sentence, detail = area, opening `automation-device-do`;
      - **"Turn it off after"** (timer), value "Leave it on" / "10 minutes", only under a Turn on action;
      - `row_action` **"Add a second action"** when there is one;
      - `row_action` **"Try it now"** (play glyph to add, or zap). Its outcome goes into the toggle's detail; no toast.
    - A gap, then red **"Delete this automation"**.
17. **`automation-two`**. The armed-Away example: "Lock Front Door" (detail "Entry") and "Run Good Night".
18. **`automation-failed`**. The toggle detail in amber: "On · didn't work at 8:02 AM: Front Door Lock didn't answer".
19. **`automation-missing`**. The action row's detail in amber: "Missing: it was called Porch Light · pick another device".
20. **`automation-newer`**. The toggle greyed with "Can't run: made by newer software". Only Delete works. Other "Can't run" details:
    - "Can't run: Basement Door can't start an automation now" (zone retyped)
    - "Can't run: open it and pick the device again" (kind changed or broken)
21. **`automation-window`**. Sheet **"Only between"**:
    - two `time_rollers` sets generalised from `night_sheet` (5-minute steps);
    - result line "From 6:00 PM to 6:00 AM";
    - button **"Any time"**; Cancel / OK.
22. **`automation-off-after`**. Pick sheet **"Turn it off after"**: "Leave it on", "5 minutes", "10 minutes", "15 minutes", "30 minutes", "1 hour".
23. **`automation-delete`**. Confirm (red OK): title "Delete this automation?", note "It stops at once. This can't be undone.", OK "Delete". Then back to the list, no toast.

**Phase C screens (`screens/automations.js`, `home.js`):**

25. **`automation-then-arm`**. `automation-then` with a group **"The alarm"** first. Row "Arm the panel" (shield), detail "At a set time, after a warning". It is absent unless the trigger is a time.
26. **`automation-arm`**. Sheet **"Arm the panel"**:
    - segment "Away | Stay | Night" (a 64 px row of three);
    - the partition row (more than one partition);
    - `row_number` "Warn first", "{0} minutes" (1-15);
    - `row_toggle` (circle-slash) **"Bypass open zones"** (D29), off by default; detail "Off: it won't arm while a door or window is open" / "Arms anyway; each one is watched again once it closes" (`automation-arm-bypass`);
    - result line "Warns at 10:25 PM, arms Night at 10:30 PM." (with the switch on: "…, bypassing open zones.");
    - note "Fire, CO and 24-hour zones are never bypassed. It does nothing if the panel is already armed. Anyone can put it off with their code."
    - Away adds the amber line "Inside sensors will be on." (`automation-arm-away`).
27. **`automation-arm-page`**. The page with Then "Arm Night".
    - Detail "Set up by Alex · warns 5 minutes before" (D30: never "Arms as Alex"; it still uses Alex's permission, re-checked each time).
    - No Try it now and no second action.
    - Toggle details: "On · didn't arm at 10:30 PM: Kitchen Window was open" / "On · already armed at 10:30 PM" / "On · put off by Pat until 11:30 PM" / "On · not tonight, put off by Pat · arms again tomorrow at 10:30 PM" (`automation-arm-skipped`) / "On · didn't arm at 10:30 PM: no time to warn first" / "Can't run: Alex can no longer arm this partition".
    - The Disarm page (`automation-disarm-page`) reads "Disarm Home" / "Set up by Alex", and under a time-of-day Disarm with no arming partner the quiet line "Nothing arms it again. Add one that arms in the evening." (section 3, disarming item 5).
28. **`home-arm-warning`**:
    - A steady amber, clickable status-bar banner "Arming Night at 10:30 PM", with no countdown and no motion. It opens Home.
      It yields to the alarm, trouble and installer-mode banners and, for its few minutes, takes the factory-code
      reminder's place (the reminder is back once the warning ends): it is the one that needs an answer now. Another
      partition's warning is named ("Garage · Arming Away at 10:30 PM"); it is put off from that partition's Home.
    - Home shows a short card **above the arming tiles**, which keep their look and stay usable (D19): title "Arming Night at 10:30 PM", line "Set by an automation", one button **"Put off"** (D26). The tiles share the height left. The Panel Manager's Overview does the same above its arm buttons.
    - Not ready (D28, `home-arm-warning-not-ready`): one amber line "Not ready: it won't arm yet"; the zones stay on the state card and the Zones screen. With "Bypass open zones" (`home-arm-warning-bypass`): "Open zones will be bypassed".
29. **`home-arm-put-off-choice`** and **`home-arm-put-off`** (D26). "Put off" opens a sheet "Put off arming?" ("Arming Night at 10:30 PM") with two large plain choices and Cancel: **"An hour later"** ("Arms Night at 11:30 PM") and **"Not tonight"** ("Arms again tomorrow at 10:30 PM"; "Not today" for an arm time between 6 AM and 6 PM - the planned one, which an hour's put-off may have moved). A choice opens the code pad titled by it ("Put off an hour", note "Any code puts it off an hour. Arms Night at 11:30 PM."; after "Not tonight" the title is "Not tonight", note "Arms again tomorrow at 10:30 PM. Needs a code that may disarm Home."). A code that can't disarm that partition gets "That code can only put it off an hour" and nothing is put off (D31).
30. **`home-arm-put-off-home`**, **`home-arm-skipped-home`** and **`home-arm-put-off-done`** (D27). After a put-off the card and the banner go; one steady grey note takes the card's place: "Arms Night at 11:30 PM" / "Put off by Pat", or "Won't arm tonight" / "Put off by Pat". The tiles don't change. The note goes when the new warning starts (with its own "Put off"), when the hold ends, or while the partition is armed by hand; it also shows beside a not-ready state. Automations says "Put off by Pat until 11:30 PM".
31. **`home-armed-by-automation`** (D30). The armed state card's foot reads "Since 10:30 PM · By automation · 1 zone bypassed", never "By Alex".
32. **`activity-automation`** (D30). Activity's rows say an automation did it and which: "Disarmed" / "By automation · At 10:00 AM on weekdays"; "Armed Night" / "By automation · 1 zone bypassed"; "Kitchen Window bypassed" / "By automation · At 10:30 PM every day"; "Kitchen Window watched again" / "Closed after arming · By automation"; "Arming put off an hour" / "By Pat · until 11:30 PM"; "Night arming skipped tonight" / "By Pat"; "Disarmed" / "By a deleted automation"; a person's "Disarmed" / "By Master" beside them.

**Kit gaps** (added to kit.js / panel.css first, named as their XML components):
- `M.settings` takes an extra-sections parameter (SECTIONS is fixed, kit.js:186-197);
- `time_sheet` (one `time_rollers` + `day_toggle` row + result line);
- `day_toggle` (64 px round);
- a non-danger `confirm_dialog` variant;
- in C: the `arm_banner` kind and `arm_strip` (the short card above the tiles, kit `M.arm_strip` / `M.panel_arm`);
- icon codepoints for any of fan, droplets, thermometer, toggle-right, play missing from `icons_20` (globals.xml:93-94).

## 10. Panel Manager page

**Menu.** Household, contiguous and mirroring the touchscreen order: People and codes, Integrations, **Automations**.

```js
{id:'automations', title: tx('Automations'), icon:'zap', group: tx('Household'), access:'system',
 render: automationsPage, when: () => !!state?.capabilities?.automations}
```

**Routes:** `#/automations`, `#/automations/new`, `#/automations/<slot>`.

**List** (copies pcListPage/pcRow, panel-pages.js:1037-1053):
- `pageHead('Automations', 'Things the panel does by itself.', [button 'Add an automation'])`.
- A notice when the clock is unset: "The clock isn't set, so times wait until it is."
- `.pc-card` per automation: the same title and detail sentences as the panel, tags "Off" / "Didn't work" / "Device missing" / "Made by newer software", and "Ran at 7:42 PM".
- **Empty:** the paragraph from screen 3 and the Add button.
- **Full:** the Add button disabled, with "16 is the most. Delete one to add another."
- **Armed:** the notice "Armed: automations can be changed once every partition is disarmed and quiet." Cards still open, read-only.

**New** (`#/automations/new`): idea cards with the same words as screen 7. Each opens the record page pre-filled. Nothing is saved until Save.

**Record page** (copies pcPersonPage/pcBody/pcDock, 1061-1299):
- A switch "Use this automation", with the status `kv` "Last run".
- Section **When**: a select of the trigger kinds (same words as screen 8). Dependent selects:
  - partition (with more than one);
  - "Armed how?";
  - zone (only usable zones, from the server);
  - for a time: a select from `dayTimes(5)` (add a step parameter to `dayTimes()`, panel-pages.js:1497) plus seven day checkboxes.
- Section **Only between**: "Any time", or two `dayTimes(5)` selects.
- Section **Then**:
  - a device select with one optgroup per area;
  - an action select fed from `actions[]`;
  - "Turn it off after";
  - "Add a second action" and "Remove this action";
  - for a scene or a switch Turn on, `review()` with the screen-15 sentences and the button "Use it".
  - Phase C adds "Arm the panel": level, partition, "Warn first" (1-15).
- **"Try it now"**, for device rules: its outcome shows in the status `kv`.
- The `pcDock`: "{0} changes · Undo · Save".
- **"Delete"** (WORD-006: the same word as the panel) asks through `review({danger})`: "Delete this automation?" / "It stops at once. This can't be undone."

**Endpoints** (`web_automations.c/.h`, U18, in the web_zwave style; the context in one PSRAM `web_alloc`, zeroized before it is freed). This is the contract the page is built against:
- **`GET automations`** →
  ```
  {maximum:16, used, gen, clock_set, read_only, less_secure,
   automations:[{slot, uid, on, record:"ok"|"newer"|"broken"|"bad",
     when:{kind:"armed"|"disarmed"|"alarm"|"fire"|"zone_opened"|"zone_closed"|"time", partitions:[1],
           levels:["away"], zone:1, zone_name:"Front Door", minute:1380, days:["mon",…], window:{from:1080, to:360}|null},
     then:[{kind:"device", source:"ha"|"zigbee"|"zwave", ref:"light.porch", device:"Porch Light", area:"Outside",
            found:true, online:true, action:"on", off_after:10, confirmed:false}],
     status:{result:"none"|"ran"|"tried"|"failed"|"skipped", tried, at, action, device, reason}}],
   zones:[{number, name, class, partition, usable_open, usable_close}],
   partitions:[{number, name}]}
  ```
  A record other than `ok` sends `when:null, then:[]`; a stored arm or disarm action (phase C, not runnable before it) sends `"newer"` (Delete only). A days mask of 0 is sent as all seven. `action` is one of on, off, toggle, lock, unlock, close, open, stop, run. The status: `ran` (or `tried` when Try it now did it); `failed` with the reason refused, no_answer, unreachable, missing, busy or not_allowed; `skipped` (nothing was sent) with held_back, cant_run, no_clock, less_secure_off or maker_gone; `tried` is true for any outcome of Try it now, failed ones too; `at` in epoch ms or null. `less_secure` is the installer option (AUT-015). Phase C adds `{kind:"arm", partition, level, warn_min, bypass}` and `{kind:"disarm", partition}` to `then`, the maker's name ("set up by Alex", D30), the put-off state (D26, D27) and `capabilities.automation_arm`.
- **`GET automations/devices`** → `{devices:[{source, ref, name, area, kind, online, actions:[{action, confirm}]}]}`: the actions from `hc_auto_offer()` with the live option, `confirm` from `auto_action_needs_confirm()` (D8, D9); devices with no offered action, scripts among them, are left out. It reads `device_service_list()` into the `web_alloc` context, never `B.dev_list`, whose static `s_dev_rows` is unlocked (ui_port_esp.c:885).
- **`POST automations`** takes `{action:'save'|'switch'|'delete'|'try', slot, gen, automation}` and answers `{result:'saved', slot (null if another edit removed it meanwhile), gen}`, `'switched'`, `'deleted'` or `{result:'tried', gen, status}`. The save maps the page's words to the model (on → ACT_ON with value -1; off → SET_MODE "off" for a thermostat, else ACT_OFF; `off_after` only with on; window from/to + 1 as stored), takes `dev_kind` from the live device whose source and ref both match (else `missing_device`), rejects a ref outside 1..95 bytes (400, never cut into another device), and saves as the signed-in user through `automation_service_save()`, which checks it against the panel as it is (never a caller's env). Errors are `409 {error, index?}` (index: which action):

  | Error | Page words |
  |---|---|
  | `changed` | "Someone changed automations meanwhile. Reload to see it." |
  | `full` | "16 is the most. Delete one to add another." |
  | `action_not_allowed` | "That action isn't allowed in an automation." (never `not_allowed`: the page's ERRORS uses that for access) |
  | `same_device` | "That device is already in this automation." |
  | `less_secure_off` | "Your installer has turned off automations that unlock, open or disarm." |
  | `needs_confirm` | none: the page asks the D24 confirm for that action and sends it again with `confirmed` |
  | `incomplete` | "Choose when it runs and what it does." (an empty days list on a time trigger too) |
  | `bad_zone` | "That sensor can't start an automation." |
  | `missing_device` | "That device isn't on the panel now." |
  | `storage_full` | "There's no room left to store it." |
  | `newer` | "Made by newer software: it can only be deleted." |
  | `read_only` | "Automations can't be changed right now." |
  | `locked` | the armed notice (the generic lock gate, before the handler; Try it now is refused while armed too) |

  400 for malformed input; 503 `failed` (no memory, storage error) or `busy` (Try it now wasn't taken in time: try again).
- `get_access`/`post_access` (exact paths) at `ACC_SYSTEM`; a grant session's POST is refused (403 `password_sign_in_required`); not in `locked_exempt`. Mirrored in mock-panel.cjs GET_ACCESS/POST_ACCESS (46-60); `tools/tests/test_panel_web_automations_access.py` pins the levels. `capabilities.automations` in `status` is false without `ALARM_FEATURE_AUTOMATIONS`.

**Mock data** (mock-panel.cjs, Harbor House):
- Devices: Porch Light (Outside), Hallway Light (Downstairs), Living Room Lamp switch (Living Room), Christmas Lights switch (Outside), Front Door lock (Entry), Garage Door cover (Garage), Water Main valve (Utility), Hallway thermostat (Downstairs), the scene "Good Night".
- Zones: Front Door, Back Door, Kitchen Window, Hall Motion, Basement Leak.
- Automations: the five list rows in section 9. Phase C adds "At 10:30 PM every day → Arm Night".
- Sign-ins: Alex (system master) sees the page; Pat (partition master) and Sam don't.
- `MOCK_LOCKED` shows the armed notice.

**Tests.**
- `panel_pages.test.cjs`: EXPECTED/RESTRICTED/ARGS for the new routes; household device and automation names in the served whitelist (777-788); refusal tests (a forbidden action → `action_not_allowed`, Pat → 403).
- The proposal's `routes()` must model the refusals itself (shim.js:195-213).
- `manager_mockup.test.cjs` renders the proposal.

## 11. Decisions

The decisions, with who took them, are in [README.md](README.md) "Decisions"; this design follows
them.

## 12. Work plan

The units are the Tasks in [README.md](README.md), each tagged with its backlog row (BL-168 phase
A, BL-169 phase B, BL-170 phase C).

## 13. Risks

- **R1 Listener slots.** Without the raise to 12 and the non-fatal registration, a ninth `add_listener` aborts the boot under `ESP_ERROR_CHECK` (panel_app.c:1654-1678) and rolls the OTA back. U10 lands the raise, the `_Static_assert` and the start-up slot log together.
- **R2 Opaque scenes and switches.** The confirm is the owner's attestation, not proof, and HA can change a scene later; the installer option doesn't reach inside them. The docs state the limit. Scripts are never allowed.
- **R2b Less secure automations (D23).** An automation can now let someone in: a door that disarms the panel lets in whoever opens it. The plain confirm says so in words at save, the installer option can take these off a site, and every run is the saver's own authority, re-checked. It was decided so (D23).
- **R3 Empty registry after boot.** dsrc_ha lives in RAM (dsrc_ha.c:148-214). Rules that fire before HA reconnects wait 30 s, then report "wasn't reachable". There is no catalog-complete signal, so "missing" relies on "up 30 s". A catalog-end flag in ha_link would make it exact. Phase B is the real cure for Z-Wave devices.
- **R4 Settings column.** Without finding 7 the tenth line scrolls and Installer falls below the fold. U14 is blocked on it.
- **R5 A missed auto-arm.** A restart during the warning leaves no record and no arm that night. The Home card and banner vanish, so a person who saw them may assume it armed. It is acceptable because it fails safe to disarmed, and the docs say so.
- **R6 Bedtime error beep and phone message** when a window is open (D4/D6). This is intended. You should hear it once in the simulator before C ships.
- **R7 Clock and DST.** The logic is subtle. Table-driven host tests use injected civil times, and DST on the panel rests on newlib's POSIX TZ, so one bench check across a TZ change is in C8. A one-hour zone change behaves like DST.
- **R8 Smart Home tile words.** Automation-sent actions are unknown to `s_dev_sent` (ui_port_esp.c:876-911), so tiles may show stale action words after an automation acts. Follow-up: keep the pending action in `hc_device_t`.
- **R9 NVS pressure.** 307 entries worst case (308 with the namespace's own, section 6) against a budget whose total worst case already exceeds 3,906. The add-time reserve is mandatory; C1 re-checks the config blob's 272-byte system term (alarm_codec.h:29-30).
- **R11 ha-addon lag.** `automation_failed` is dropped silently until U20. The add-on already misses 23 names.
- **R12 Locks.** A future change that calls device_service under `s_auto_lock`, or takes `board_ui_lock` from the auto task, would deadlock against `port_result` or the LVGL save path. The rule (copy out, then call; mailbox for the UI) is a review item.
- **R13 PSRAM stack.** The auto task must never write NVS (flash auto-suspend is off). Saves stay on the callers' internal stacks; U10's review checks it.
- **R14 Phase A alone** still needs Home Assistant devices, so HCTL-009's "without Home Assistant" is true only after B. The status lines say so until then.
- **R15 Image size.** Estimated: core, service, UI and web about 45 KB, plus about 8 KB gzip of web, within one 64 KiB step (68.1 % → about 69.5 %, limit 85 %). Measured after U10 (the core and the service): 69.5 %, internal RAM +1 KB; again after U18 and C4.
- **R16 Latent tile bug, outside automations.** Smart Home's tap sends `ACT_ON` value 0 (ui_devices.c:440), which reaches HA as brightness 0. Automations use −1; the tile path needs checking in the ha-addon separately.
- **R17 Host and panel glue drift.** `host_panel.c` and the service wire the same core differently. U8's end-to-end test and the preview walk limit it; U22 and C8 are the real proof.
- **R18 Try it now's wait.** A caller is told "busy" after 100 ms unless the task has already taken its request; then it waits for `auto_run_try` to return, which only queues sends (bounded in practice, not by a timeout). The touchscreen holds `board_ui_lock` meanwhile; a panel test measures it (U22).
- **R19 A read-only store has no way out.** An unreadable key or a meta this firmware can't read makes the store read-only (section 6), and every edit is refused, delete included, so only a factory reset clears it. A way out (an erase of the namespace from the installer's menu, say) is still to be decided; the console only lists (AUT-010).

## 14. What we learned from other systems (research, 2026-10-05)

The brief: learn from Home Assistant without copying it, and from alarm systems in
general, but stay more relaxed than they are. Where a claim rests on a forum reply or a secondary
list it says so.

**Put-off.** Every classic panel lets a code push an automatic arming later and logs who did it:
any code, or a master or manager only; by a set number of minutes, an hour or two, or a new time
typed in, never past midnight; each logged, and the central station told. None has a "skip tonight"
that ends by itself, so "Not tonight" (D26) is new; a consumer system's pause and per-rule switches
must be undone by hand. Contact ID 464 "auto-arm time extended" and SIA CE carry the meaning; we
send neither. Adopted: a put-off on every warning, logged with who (section 7); the steady line on
Home (D27). Proposed: "Not tonight" needs a code that may disarm (a limit classic panels set too).

**Not ready and bypass.** Forced arming on an automatic arm is standard and an installer's setting
(otherwise "auto-arm failed", Contact ID 455), and a consumer DIY system bypasses "by a schedule"
in its history. UL listings forbid it; CP-01's public summaries say nothing about automatic
arming. No panel bypasses fire or CO. The known trap is a bypass that lasts all night after the
door shuts (a garage case, from one support-forum thread): some panels watch a force-armed zone
again once it closes, and so does ours (section 3, item 6). Reports carry "partial closing"
(Contact ID 456) or "was force armed": hence the ARMED mark "1 zone bypassed". Our engine already
keeps fire, CO and 24-hour zones out of readiness (zones.c:28-37), but its bypass would also take a
sensor in trouble, so the automation's bypass is narrowed to open zones.

**Who did it.** Panels and the reporting formats treat a rule as its own actor (an "armed
automatically" mark, an automatic closing by user 000), and Home Assistant's logbook shows "By
automation" and its name from the context's parent (an automation's context has no user). An
alarm integration that names a person for an automation is the counter-example. Hence D30
everywhere, not only in Activity (section 7), and "changed since" for a line older than an edit
(our own fix: the uid survives an edit).

**Home Assistant's habits we keep.** One run at a time (mode "single" ignores a new trigger while
one runs, and turning an automation off stops the run): one warning at a time, and turning it off
ends it. HA's actionable notifications pair a phone button with authentication; ours has none in v1.

**Left to Home Assistant** (it does them well; we don't duplicate them): presence and geofence
arming (alarm apps and consumer DIY systems only remind); "quiet for 30 minutes" and other "for at least"
conditions; more conditions, several triggers, branches, waits and templates (alarm integrations
make the same split); put off or "arm anyway" from a phone; holiday and workday calendars until SCH-001; a lock's
keypad trigger ("unlocked by code, so disarm"); scripts, setpoints, brightness and media.

**Where we are more relaxed, and the rail we keep:**

| Others | They restrict | We allow | We keep |
|---|---|---|---|
| Alarm apps, voice assistants, classic panels, UL listings | No disarm by schedule or routine, or only paired with an arming one | A disarm from any trigger, made by the master or installer | The installer option; the maker's authority, re-checked; never while an alarm sounds; logged "By automation"; the nudge toward an arming partner |
| Voice assistants, alarm apps | No unlock or open in automations; nothing lowering after an arming or locking trigger | Unlock, open and stop as normal actions | The installer option; the plain confirm after a door, window, motion or alarm trigger; the 10 s device gap; no Disarm after its own armed trigger |
| Alarm apps' smart arming | A sensor disarms only from inside motion, once a day, one partition | Any door, window or motion zone may start a disarm | The confirm in words; the server checks it; the installer option |
| Alarm apps' smart arming, classic panels | Arming only Stay or only Away, one partition | Away, Stay or Night from any trigger, any partition | A whole warning; only from disarmed; never another level; put off with a code |
| UL and EN listed panels | No forced arming on an automatic arm | "Bypass open zones" | Off by default; open burglary zones only; fire, CO and 24-hour never; logged and reported; watched again once closed; all clear at disarm |
| Classic panels | Only master, manager or installer extend an automatic arming | Any code puts it off an hour | Wrong codes count toward lockout; duress reported silently; every put-off logged; proposed: a skipped night needs a disarm code |
| Classic panels (installer software), alarm clouds | Rules made only in installer software or the cloud | The master makes them at the panel or in the Panel Manager | Master or installer only; never while armed or in alarm; the server checks every save |

**Sources.** Home Assistant's context data, automation modes, logbook processor, frontend strings
and Companion actionable notifications; the Contact ID and SIA DC-03 code lists (secondary);
summaries of SIA CP-01.
