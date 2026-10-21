# 20 · Zigbee: sirens, keypads and key fobs

Part of [docs/features/20](README.md) (requirements ZB-023 and ZB-024; tier B of the
scope of 2026-10-02, milestones ZB-M10 and ZB-M11, rows BL-147 and BL-148). How Zigbee devices
that are part of the alarm itself - sirens and strobes (IAS WD), keypads and key fobs (IAS ACE) -
are driven by the panel, and the rules that keep them from weakening it. Device management is
[devices.md](devices.md); the tests are [testing.md](testing.md) §3, §4 and §9. Contracts:
`components/core/zigbee_zones/include/zigbee_zones/zbz_wd.h` and `zbz_ace.h`.

## 1. The rules

- **The engine decides; the radio carries.** A siren sounds because the engine's bell output is
  on, never because a radio frame said so; a keypad arms or disarms only through an engine
  command with a code the engine checks. Only the alarm task touches the engine: its outputs and
  state reach the zigbee task as posted messages, and a keypad's command reaches the alarm task
  the same way (`zba_post_t`). Stack callbacks only queue.
- **Codes are the panel's.** The Zigbee side never compares, keeps, logs or shows a code. A
  keypad's code travels once, in the posted command, and the posted copy is wiped.
- **The lockout applies.** A keypad is `ALARM_SRC_KEYPAD` with its engine device number as id:
  five wrong codes lock it out like any keypad (the engine's own counter). The panel gives the
  number, from those no other keypad of any kind holds (§4).
- **Off by default where it matters.** A key fob disarms only when the installer turned that on
  for that fob; its panic button only when turned on.
- **No new trouble types.** A siren in trouble - its silence included - is the siren trouble the
  engine has (`ALARM_TRBL_BELL`); keypads and fobs add none, and nothing writes the engine's
  device table (§4).
- **Nothing on the panel flashes.** A siren's strobe is the siren's; the pages say "strobe" in
  words.
- **Changes need installer mode with every partition disarmed**, as every Zigbee change; a
  siren's test too (devices.md §1).

## 2. Sirens and strobes (IAS WD, ZB-023)

**Which devices.** A device with an IAS WD server (cluster 0x0502), usually device id 0x0403 and
an IAS Zone of type 0x0225 on the same endpoint: Frient SIRZB-110/111 (endpoint 43), Heiman
HS2WD (also sold as Tuya TS0216), smoke detectors with a built-in sounder (Heiman
SmokeSensor-EM). Tuya and Neo sirens that speak only the manufacturer cluster 0xEF00 are not
supported (the quirk says "use Home Assistant"): the decision is the clusters', not the brand's.

**The assignment** (page 14, `ui_zb_siren_t`, stored as `zbpanel/wd`): the partitions it sounds
for; what it sounds for - burglary (and audible panic) and/or fire (fire and CO); its strobe with
it; a chirp when the partition arms and disarms; volume (low to very high; hidden for a model
that ignores it); how long it may be silent before it is a trouble (§4); and the partitions
whose alarms it also sounds for (`also`, docs/features/16 OUT-015: their alarms and strobe, never
their squawks or bell tests). Up to `ZBW_SIRENS_MAX` (4). A siren takes no engine device number:
the panel supervises it itself (§4). A smoke detector whose sounder is an IAS WD offers the same
settings on its own page and stays a sensor (BL-215).

**Decided in building it (siren page).** Two switches, "Burglary and panic" and "Fire and CO"
(not three: panic rides with burglary); CO sounds the siren's emergency tone, fire its fire
pattern. "Test it" sounds it for 2 seconds (`ZBW_TEST_S`) at its own volume, and is refused while a
partition is armed. A siren has no "Replace it" (it feeds no zone; a replaced device loses its
role, §4): its page offers Remove and Forget.

**How it sounds.** The alarm task's `on_output` for `ALARM_OUT_BELL` and `ALARM_OUT_STROBE` is
posted to the zigbee task (`zbz_panel_output` -> `zbw_output`):

| Engine | Warning mode | Notes |
|---|---|---|
| `ALARM_PAT_STEADY` (burglary, audible panic) | burglar (1) | only sirens with `burglary` |
| `ALARM_PAT_TEMPORAL3` (fire) | fire (2) | only sirens with `fire` |
| `ALARM_PAT_TEMPORAL4` (CO) | emergency (3) | a siren has no CO sound of its own; only with `fire` |
| `ALARM_PAT_TEST` (bell test) | burglar, `ZBW_TEST_S` | |
| `ALARM_PAT_OFF` | Stop (0) | |
| `ALARM_OUT_STROBE` alone | Stop with the strobe bit | strobe-only; whether a model flashes on mode 0 is a bench check |

A model flagged one-tone (Heiman HS2WD: emergency and stop only) sounds emergency for everything.
Medical and auxiliary alarms sound only the panel's own keypads.

The engine owns the timing. A siren doesn't say when its warning ends, so the panel asks for the
shorter of the siren's MaxDuration (read at its setup; default 240 s, a model row may cap it)
and `ZBW_WARN_MAX_S`, and asks again early enough for every retry to land before it runs out
(`ZBW_REFRESH_LEAD_MS`) while the output is on;
when the engine's bell times out or is silenced the output goes off and the panel sends Stop.
Fire keeps its temporal-3 on the panel's own sounder (NFPA 72); a Zigbee siren is additional
notification, never the only one.

**Squawk.** When a partition arms or disarms (the engine's ARM and DISARM events, posted), each
siren of it with `squawk` gets a Squawk: armed or disarmed mode, its strobe if `strobe`, at its
volume.

**Test it.** `ZBW_TEST_S` (2 s) of its burglar sound, refused while any of its partitions is
armed; the result shows on its page (`last_test`).

**The frame.** Start Warning: Warning Info (spec layout: mode bits 7-4, strobe bits 3-2, level
bits 1-0), duration u16, strobe duty cycle (`ZBW_STROBE_DUTY` 50 %), strobe level. Open
implementations disagree on the bit order for some models: Zigbee2MQTT packs Develco/Frient
sirens reversed (mode in the low nibble, strobe bit 4, level bits 7-6), while deCONZ's SIRZB-111
file sends the spec layout. The model row carries the layout (`ZBZ_MODEL_WD_REVERSED`), and one
real siren is captured on the bench before a siren ships (testing.md §4).

**Confirmed or trouble.** A Start Warning or Stop asks for a Default Response (unless the model
never sends one); not confirmed after `ZBW_SEND_TRIES` tries `ZBW_RETRY_MS` apart, or its own IAS
Zone status shows trouble, AC mains lost or a battery defect, or it has been silent too long
(§4): `ALARM_TRBL_BELL` (posted). It clears when every siren is confirmed again, heard again and
its status is clean.

**Decided in ZB-M10** (2026-10-02, without waiting for review; built in
`zbz_wd.c`, proven in `test_zbz_wd.c`):

- **Bit order, for now.** Every siren gets the spec layout until a bench capture says otherwise:
  the ZCL specification, zigpy/ZHA and deCONZ's SIRZB-111 file agree on it, and only
  Zigbee2MQTT packs some Develco/Frient models reversed. No model row sets
  `ZBZ_MODEL_WD_REVERSED` before that capture (§6). The reversed layout is the spec's fields
  read from the low bit up (mode bits 3-0, strobe bit 4, a reserved bit 5, level bits 7-6) - the
  one reading that explains Zigbee2MQTT's packing - and the flag applies it to the Squawk byte
  too. Bits a layout gives no field are kept as they came (`reserved`), so every byte decodes and
  encodes back to itself.
- **Several partitions.** A siren gives the most urgent bell of the partitions it sounds for:
  fire, then CO, then burglary; when that one stops it goes back to the next. A bell pattern the
  table doesn't name (a bell simply "on") sounds burglar.
- **Strobe.** With `strobe` chosen the strobe runs with every warning, at duty
  `ZBW_STROBE_DUTY` and strobe level high (the assignment has no strobe level), and on its own
  (Stop with the strobe bit) while any of its partitions' strobe output is on, whatever the
  alarm was. Stop is mode 0 at its volume, no strobe, duration 0.
- **Asked again.** `ZBW_REFRESH_LEAD_MS` (51 s: `ZBW_SEND_TRIES` tries, each unanswered for up
  to `ZBW_CONFIRM_MS`, `ZBW_RETRY_MS` apart; at least `ZBW_REFRESH_S`) before the warning runs
  out, so a refresh that fails is retried - and the bell trouble raised - while the siren still
  sounds; at its half when its MaxDuration is shorter than twice that (there the last tries may
  land after a short gap). The running warning's end counts from the last send.
- **Confirmations.** A Start Warning or Stop not confirmed is sent again `ZBW_RETRY_MS` later; a
  send whose end the radio never tells counts as failed after `ZBW_CONFIRM_MS` (15 s). After
  `ZBW_SEND_TRIES` failures it is the bell trouble and the command is tried every
  `ZBW_RETRY_SLOW_MS` (10 s) while it is still wanted, until one is confirmed. Failures while the
  radio is down don't count (`sensor_radio` says it); when it comes back the command goes at once.
- **Squawk and tests are never held against a siren.** A squawk isn't confirmed or retried (a
  late chirp would mislead) and isn't sent by a siren giving a warning or with a command not yet
  confirmed - on a disarm that silences an alarm the Stop is the feedback. "Test it" and the
  engine's walk-test pulse (`ALARM_PAT_TEST`) give `ZBW_TEST_S` of the burglar sound (emergency
  on a one-tone model) to a quiet siren, once; "Test it" with its strobe if chosen, and refused
  `ZBW_ERR_ARMED` or, while it gives a warning, `ZBW_ERR_BUSY` (both appended to
  `zbw_result_t`). Their outcome counts only as a frame heard; a siren that can't be reached
  shows through its supervision.
- **Its own status.** Only the IAS Zone trouble, AC mains and battery-defect bits are the bell
  trouble; tamper and a low battery are not (the siren's zone, if it feeds one, reports those).
- **The store.** A damaged `wd` blob is refused whole (as the install codes) and leaves the
  sirens as they were - none at start-up; a newer compat is refused; a longer entry's later
  fields and flag bits 4-7 are skipped. A good blob replaces the live sirens without a Stop:
  `zbw_decode` is a start-up call, and a restore at run time clears sounding sirens first.
- **Leaving.** A siren cleared, or moved to another address or endpoint, gets Stop at its old
  address when it gives a warning or its last warning or Stop isn't confirmed yet (not followed
  up); a moved one starts as a new siren at the new address.
- **Wired in zbz_panel** (BL-147, decided 2026-10-03 without waiting for review;
  proven in `test_zbz_panel.c` against the fake radio and the real engine):
  - *The panel's own port.* `zbz_panel` owns the `zbw_t` and gives it a port of its own: `send`
    is the radio's `zcl_command` under the siren's `'W'` tag (never `ZSRC_ZB_TAG_SOURCE`, refused
    while the radio isn't working); `bell_trouble` goes to the owner's `wd->bell_trouble`, which
    posts `ALARM_TRBL_BELL` to the alarm task. The owner's `send` and `checkin` are not used.
  - *The engine's news.* `zbz_panel_output` is `zbw_output` (the engine owns the timing);
    `zbz_panel_partition` gives `zbw_set_armed` for a partition's first view (start-up: no chirp)
    and `zbw_armed` when its armed state changes (armed = any state but disarmed, the exit delay
    included).
  - *The radio.* `zbw_radio` follows the radio's state (and is set again by a load while the
    radio works); `zbw_poll` runs with the poll and `zbw_next_deadline` joins
    `zbz_panel_next_deadline`; `REP_DONE` with a tag under `ZBW_TAG_MASK` goes to
    `zbw_send_done`.
  - *Heard.* `zbw_heard` for any report from a device's address that is its frame - not its
    leaving, a refused join, nor a request that failed. `zbw_zone_status` is not wired: a siren
    has no zone source slot, so its IAS Zone status never reaches zbz_panel (left for a later
    unit with the radio side's raw status).
  - *"Test it"* (`SIREN_TEST`) is an operation (`UI_ZB_OP_SIREN_TEST`): refused while any of its
    partitions is armed (`NOT_ALLOWED`), while its last command isn't confirmed (`BUSY`), and for
    a device that isn't one of the panel's sirens (`INVALID`). zbz_panel follows the test's own
    tag: its `REP_DONE` OK is `DONE`, TIMEOUT `NO_ANSWER`, anything else `FAILED`; no end within
    `ZBZ_SIREN_TEST_MS` (`ZBW_CONFIRM_MS`) is `NO_ANSWER`, the radio lost `FAILED`. The result is
    the page's `last_test`, kept in RAM by device id (not stored), so no hook was added to
    `zbw_port_t`.
  - *SIREN* with no partition clears it (Stop if it sounds); otherwise its settings with the
    IAS WD endpoint, `model_flags` and `max_duration_s` from the model table and `sleepy` from
    the record. `zbw_assign`'s ARG is `INVALID`, FULL `FULL`.
  - *Leaving and the store.* A device removed, forgotten or replaced, or the network reset,
    loses its assignment (`zbw_clear`) and its test result. `zbz_panel_take_wd` encodes when
    changed; a load decodes `wd`, refills `model_flags`, `max_duration_s` and `sleepy` through
    `zbw_assign`, and leaves nothing to write back. A damaged or later value loads no siren and
    isn't written over.

## 3. Keypads and key fobs (IAS ACE, ZB-024)

**Which devices.** IAS ACE clients (output cluster 0x0501): keypads - Frient KEPZB-110 (endpoint
44), Centralite 3400 and 3400-D, Xfinity XHK1-UE, Linkind ZS130000078 - and key fobs - Heiman
HS1RC, Linkind ZS130000178. The panel is their ACE server. Keypads that speak Z-Wave's Entry
Control instead are docs/features/36's.

**Keypad assignment** (page 15, `ui_zb_keypad_t`, stored as `zbpanel/ace`): the partition it
arms and disarms; which of its emergency buttons may raise what (`panic_mask`: 1 <<
`alarm_panic_t` - police, fire, medical; off unless chosen); whether the panel's state is sent
to it. Its engine device number is not asked for: the panel gives it one when it is assigned
(§4) and the page shows it. Up to `ZBA_KEYPADS_MAX` (8), fewer when the panel-link keypads or the
installer's device table hold some of the numbers 1-8.

**Decided in building it (keypad page).** The page shows "Keypad number", the one the panel gave
it. Each emergency button offers Off or its one alarm (Panic: an audible panic alarm, Fire, Emergency:
medical); a keypad has no silent panic. The partition may be Off (it then arms and disarms
nothing). A keypad and a key fob have no "Replace it", like a siren: Remove and Forget only.
The Panel Manager offers the same three (audible panic, fire, medical; decided after the review
of 2026-10-03: its "Panic alarm, silent" choice was a button that raised nothing, since `zbz_ace`
maps a keypad's Panic only to the audible panic). Owed in `zbz_ace`: a keypad's `panic_mask` with
the silent bit is refused INVALID (only POLICE, FIRE and MEDICAL, 0x0D), so no request can store it.

**Who may own a key fob** (decided after the review of 2026-10-03, without waiting for review): a master, partition master, standard, arm-only or guest user - never the installer, a
duress code (every press would be a silent duress event) or an empty slot. Both pages offer only
those (the touchscreen's picker and the Panel Manager's by the user's authority, not by number).
Owed in `zbz_panel`'s `req_fob`: the same rule through the port's view of the user's authority,
refused INVALID.

**Key fob assignment** (page 16, `ui_zb_fob_t`): the user it belongs to (Alex), the partition it
arms, whether it may disarm (**off by default**), whether its panic button raises a panic (**off
by default**). Up to `ZBA_FOBS_MAX` (16). A fob carries no code: it acts as its user
(`alarm_engine_command_as`, the same rules as that user's code - an arm-only user's fob never
disarms, whatever the switch says). A fob has no engine device number.

**A fob's source** (decided 2026-10-02 without waiting for review): `alarm_types.h`'s
"keyfob button" kind, `ALARM_SRC_ZONE`, with id 0 - it feeds no zone, and 0 is "none"
(`ZBA_FOB_SOURCE_KIND`, `ZBA_FOB_SOURCE_ID`). The command runs as its user, so the log and the
Activity page say who ("By Alex"), the engine's rules for a keyfob hold (auto-stay is for keypad
arming only, in the engine) and no keypad id or device number is used. It replaces the
provisional keypad ids `0x2000` + slot, which broke `alarm_types.h`'s rule that a keypad's id is
its device number. A fob carries no code, so there is nothing to lock out; an event's source zone
0 reads as no zone wherever events are shown or reported.

**An Arm** (command 0x00: mode, code, zone id):

1. Not from an assigned keypad or fob: Default Response NOT_AUTHORIZED, nothing else.
2. The same device and sequence number again within `ZBA_RETRY_WINDOW_MS` (60 s): its first
   answer is sent again, nothing runs again (fobs repeat an unanswered Arm).
3. A bad frame (a code length past the end, a code longer than `ALARM_CODE_MAX_LEN`): Arm
   Response "invalid code"; a mode above 3: "not ready".
4. Otherwise an `alarm_cmd_t` is posted to the alarm task: mode 0 DISARM, 1 ARM STAY, 2 ARM
   NIGHT, 3 ARM AWAY, for the keypad's partition, with its code (a keypad) or as its user (a fob;
   a fob's disarm only when `may_disarm`, else "invalid code" at once). At most
   `ZBA_PENDING_MAX` (4) wait; more get no answer (the device retries).
5. The engine's answer comes back (`zbz_panel_ace_result`): the Arm Response, with the request's
   own sequence number - 0 disarmed, 1/2/3 armed, 4 invalid code (also locked out, not
   authorized), 5 not ready, 6 already disarmed (a disarm of a disarmed partition). No answer in
   `ZBA_ANSWER_MS`: none is sent.

**Emergency, Fire, Panic** (0x02, 0x03, 0x04): medical, fire and police panic for the keypad's
partition when its `panic_mask` allows (a fob: police when `panic` is on), posted as
`ALARM_CMD_PANIC`; otherwise Default Response NOT_AUTHORIZED.

**The panel's state on the keypad.** On every change of its partition (the alarm task posts the
partition's view: state, level, ready, delay seconds, alarm cause) each keypad with `show_state`
gets Panel Status Changed: 0 disarmed (6 not ready when something is faulted), 1/2/3 armed stay,
night, away, 4 exit delay and 5 entry delay with the seconds left and audible 1, 7 in alarm with
the alarm status (1 burglar, 2 fire, 3 emergency, 4 police panic...). Get Panel Status is
answered the same way with the seconds still left at that moment. Model flags: Centralite
3400-D wants 0x80 for any armed state; Frient's KEPZB-110 wants audible 1 to beep; some keypads
send Get Panel Status when touched (answered at once, they sleep within seconds); Linkind's
keypad needs one Panel Status Changed after joining.

**Bypass and the zone questions** are answered, never ignored: Get Zone ID Map gives an empty
map, Get Zone Info "no such zone", Bypass a Bypass Response "not allowed" for each zone asked -
bypassing stays on the panel's own keypads.

**The keypad's own IAS Zone** (tamper, low battery) shows on its page; how it is supervised is
§4.

**Frames and the details ZB-M11 settled** (decided 2026-10-02 without waiting for review; built in `zbz_ace.c`, proven in `test_zbz_ace.c`):

- *No Default Response asked.* Every frame the panel sends as the ACE server has frame control
  0x19 (`want_response` off): the Arm Response and the other answers are responses already, a
  Default Response to them is wasted air time for a sleeping keypad, and zigbee-herdsman's
  `commandResponse` sends ACE answers the same way. testing.md §3's rows written with 0x09 read
  0x19 (the Arm Response `19 57 00 | 03`, Panel Status Changed `19 58 04 | 04 1E 01 00`, the Get
  Panel Status answer `19 57 05 | 07 00 01 03`).
- *Default Responses* (a refusal, a panic button's answer) are global commands, which
  `zsrc_zb_zcl_t` can't carry through `send`: `zba_port_t.default_rsp` sends them; an owner that
  answers through the stack's own Default Response instead leaves it NULL and reads the action
  (§6, question 2). A panic button's success is answered only when the frame asked; a refusal
  always (NOT_AUTHORIZED, or FAILURE).
- *An assignment is the device's address and its endpoint both*: a frame from the same address
  at another endpoint is not assigned (a bench check for each model's endpoint).
- *Codes.* Only digits, at most `ALARM_CODE_MAX_LEN`, are passed on; anything else is "invalid
  code" at once without reaching the engine (it can't be anyone's code). A keypad's Arm without
  a code is "invalid code": a Zigbee keypad never quick-arms. A code a fob sends is never passed
  on: the fob acts as its user alone.
- *A fob's panic button* raises police panic whichever of Emergency, Fire or Panic it sends
  (Heiman fobs send Emergency, Linkind's Panic).
- *A silent alarm never shows on a keypad* (silent burglary, silent 24-hour, hold-up panic): it
  keeps showing the arm state, since whoever stands at it may be the reason for the alarm.
  Alarm status: burglary, exit error, tamper and audible 24-hour are burglar; fire is fire;
  police, fire and medical panic are 4, 5, 6; CO, the auxiliary 24-hour zones and anything else
  are emergency (ZCL has no CO).
- *Audible notification* is 1 on the delay and in-alarm frames and 0 otherwise; a
  `ZBZ_MODEL_ACE_AUDIBLE` keypad (Frient) gets 1 on every frame. `ZBZ_MODEL_ACE_ARMED_80`
  (Centralite 3400-D) shows 0x80 for armed, entry delay and in alarm, 0 for the rest, exit delay
  included, as zigbee-herdsman-converters do (provisional until a bench capture).
- *Panel Status Changed* isn't sent again when nothing it shows changed (a delay always is), so
  a sleeping keypad isn't woken for nothing; one the radio didn't take is tried at the next news.
  `zba_greet` sends the current one regardless (after assigning, after a rejoin); an
  `ZBZ_MODEL_ACE_INIT` keypad not yet greeted is greeted before its first answer.
- *Retransmissions*: one still waiting for the alarm task sends nothing (its answer comes); one
  answered within the 60 s gets that answer again; Emergency, Fire and Panic the same. A
  retransmission is the same device, sequence number and command - an Arm also its arm mode
  (BL-143, 2026-10-03): an Arm that reuses the number of the last one with another mode is a new
  command.
- *A panic always finds room* (BL-143, 2026-10-03): with `ZBA_PENDING_MAX` posts waiting, a
  panic takes the place of the oldest waiting Arm. That Arm still runs in the alarm task (it was
  posted already); its answer finds no ticket and its retransmission is neither answered nor run
  again (the keypad shows its own error). A panic is BUSY only when panics fill every place.
- *One fob for each user* (`ZBA_ERR_IN_USE`, the page's `UI_ZB_REQ_IN_USE`); a device is a keypad
  or a fob, and assigning it as the other kind ends the first assignment (a keypad's number is
  then free).
- *The stored value* (`zbpanel/ace`): an entry's length byte counts the bytes after it; a later
  format appends fields inside an entry and adds kinds, which this reader skips. A truncated
  value, a later compat or bytes left over load nothing; an unusable entry (out-of-range or
  doubled device number, user or partition 0, an id or address seen before) is dropped and the
  rest load. Frozen as `test/ace_fixture_v1.h`.
- *Wired in zbz_panel* (BL-148, decided 2026-10-03 without waiting for review; proven
  in `test_zbz_panel.c` against the fake radio and the real engine):
  - *The panel's own port.* `send` is the radio's `zcl_command` under the keypads' `'A'` tag;
    `default_rsp` is the owner's `ace->default_rsp` (the stack's Default Response), always with
    the request's own sequence number, never `ZSRC_ZB_TSN_NEW`. Without one the refusals are the
    owner's (`zba_port_t`: NULL).
  - *Frames.* Every `REP_ZCL` of cluster 0x0501 from a client that isn't a Default Response
    (command 0x0B: no IAS ACE client command has that id) goes to `zba_receive`, a device not in
    the registry included (it is refused as not assigned).
  - *To the engine only through the alarm task.* A `ZBA_POST` goes to the owner's
    `zbz_panel_port_t.ace_post` (appended to the port), which copies it into the alarm task's
    queue; the panel wipes its copy of the code at once. One copy outlives that today: the
    radio side's event queue (`radio_port.c`, in PSRAM) keeps an ACE frame's bytes in its ring
    until a later event overwrites the slot (found in the review of wave 6; the fix - carrying the
    payload outside the queue, or zeroing the slot - is zigbee_service's, still open). A post the owner couldn't queue (false,
    or no `ace_post`) is forgotten - its place freed - so the keypad's retry runs again.
    `zbz_panel_ace_result` is `zba_result`, by ticket.
  - *The partition's news.* `zbz_panel_partition` calls `zba_panel` only when something the
    keypad shows changed (state, level, ready, delay, in alarm, cause) or for a partition's first
    view - not for its time alone. `zba_poll` runs with the poll and `zba_next_deadline` joins
    `zbz_panel_next_deadline`; a `REP_DONE` under `ZBA_TAG_MASK` is not followed (a keypad asks
    again).
  - *KEYPAD* with no partition clears it (its number is free again); otherwise its number is
    its own if it has one, else `zba_free_device` with the owner's `devices_taken` (none:
    `FULL`, the page's `NO_DEVICE_SLOT`) - never the request's - its ACE client endpoint and
    `model_flags` from the model table, then `zba_greet`. *FOB* with no user clears it;
    otherwise `zba_assign_fob`: another user's fob is `IN_USE`, a user or partition past the
    panel's `INVALID`. A rejoin or re-join of an assigned keypad greets it again.
  - *Leaving and the store.* A device removed, forgotten or replaced, or the network reset,
    loses its keypad or fob assignment. `zbz_panel_devices_held` reports the keypads' numbers.
    `zbz_panel_take_ace` encodes when changed; a load decodes `ace` and refills each
    assignment's `model_flags` from `zbz_model_for`, leaving nothing to write back.
  - *The pages.* The device list's kind is the assignment's (siren, keypad, fob) before the
    role; a page's `live` is "assigned and still this address"; a siren's `device` is always 0.

## 4. Device numbers, supervision and troubles

Decided 2026-10-02 without waiting for review (the ZB-M1 architecture review found that
keypads and sirens taking the engine's device numbers collided with the Z-Wave keypads' and
didn't fit: 4 sirens + 8 Zigbee keypads + 8 Z-Wave keypads is 20 numbers, and the engine has 16).

- **The numbers.** The engine has `ALARM_MAX_DEVICES` (16) device numbers. A keypad's number is
  its source id (`ALARM_SRC_KEYPAD`): its lockout counter and the log's source, so two keypads
  must never share one. The top eight, 9-16, are the Z-Wave keypads' (`zw_keyctl.h`
  `ZWKC_DEVICE`, docs/features/36 ZW-031). The numbers 1-8 are shared by the panel-link keypads
  (docs/features/11 LINK-001) and the Zigbee keypads (`ZBA_DEVICE_FIRST`..`ZBA_DEVICE_LAST`).
  Sirens and fobs take none. Zigbee and Z-Wave keypads together are at most 16; the limits are
  static asserts in `test_zbz_contract.c`.
- **The panel gives them, never the installer.** When the KEYPAD request assigns a keypad,
  `zbz_panel` gives it the lowest number of 1-8 that no other Zigbee keypad holds and that the
  port's `devices_taken` doesn't name (the panel-link keypads' numbers and any the installer
  turned on in the engine's device table); the request's `keypad.device` is ignored. It keeps the
  number while it stays assigned; clearing, removing, forgetting or replacing it, or a network
  reset, frees it. None free: the keypad page shows `UI_ZB_FIT_NO_DEVICE_SLOT` and the request
  answers FULL. The other users of 1-8 read the Zigbee keypads' numbers from
  `zbz_panel_devices_held` and skip them (the panel-link pairing, LINK-004, and the installer's
  device table: docs/features/11 LINK-001 says so).
- **Nothing writes the engine's device table.** As with Z-Wave's keypads, a Zigbee keypad's
  number is a source id only: `alarm_config_t.devices[]` stays the installer's. Writing it from
  the zigbee task would race the installer's own configuration edits (both replace the whole
  configuration through the alarm task) and put radio devices into the configuration and its
  backups; nothing needs it, since nothing below uses the engine's device supervision.
- **Sirens are supervised by the panel.** Any frame from a siren is heard (`zbw_heard`); silent
  past its limit while the radio works - the installer's (`supervision_min`, an hour or more),
  else `ZBW_SILENT_ROUTER_S` (90 min, a mains siren, which routes) or `ZBW_SILENT_SLEEPY_S` (3 h,
  a battery one), the zone source's own supervision of the same kind of device and the hub's
  grace after it (devices.md §7) - it is `ALARM_TRBL_BELL`, like a warning it
  didn't confirm or its own trouble bits (§2): a siren that may not sound is the bell trouble.
  Its next frame clears it. A listening (mains) siren sends little by itself - its setup asks it
  for nothing more often than its battery report (12 h) - so the panel reads one attribute of it
  (Basic's ZCL version, as Ping does) each third of its limit it has been quiet, and the answer
  counts as heard: a quiet, working siren is never the bell trouble, a dead one still is at its
  limit (decided in the review of wave 6; to be checked on the bench with a real siren). A sleepy
  siren can't be read: its own check-ins count.
  It can't be turned off: a siren that may not sound must be known
  (the BL-145 proposal's "Off" choice goes). While the radio is down no siren counts as silent; the radio's own
  trouble (`sensor_radio`) says so.
- **Keypads and fobs are not supervised by the engine.** They give commands and guard nothing;
  losing one leaves the panel's own keypad. A keypad's silence shows on the Zigbee pages (not
  heard, unreachable: ZB-011), and when its own IAS Zone (its tamper) feeds a zone, as that
  zone's CHECK (ZB-005), as for any sensor.
- The radio itself down is the `sensor_radio` trouble as today.
- A removed, forgotten or replaced device loses its assignment, and a keypad its number.

## 5. Events

Arming and disarming from a keypad or fob are the engine's own ARM and DISARM events, with the
keypad's source (or the fob's user); wrong codes and lockouts are the engine's as for any keypad.
Assigning or clearing a siren, keypad or fob is a configuration change logged as the installer's
(ZB-M10/ZB-M11 decide whether it needs an event type of its own; if so it is appended, devices.md
§11). ZB-M11 adds none for keypads and fobs (decided 2026-10-02): assignments change only in
installer mode with every partition disarmed, whose entry and exit the engine logs, and every
arm, disarm, wrong code, lockout and panic they lead to is the engine's own event with the
keypad's number or the fob's user. An event of its own (say `zigbee_assigned`) can be appended
later without a format change if the assignment itself is wanted in the log.

## 6. Open questions

1. The Warning Info bit order of the first real siren (§2): captured on the bench before
   BL-147 ships. For now (ZB-M10): the spec layout for every model, the reversed one built and
   tested but set on no model row; the capture decides whether a row gets it. The same capture
   checks whether a siren flashes on Stop with the strobe bit (strobe-only), and the Squawk byte:
   the reversed Squawk (strobe bit 4, level bits 7-6) is a guess with no source - Zigbee2MQTT
   packs the Squawk in the spec order even for the models whose warning it reverses.
2. Does esp-zigbee-lib hand the application the raw ACE command (so `zba_decode_arm` sees the
   bytes), or only its parsed form through the ACE server callbacks (`zcl_core.h`)? If only
   parsed, `radio_port.c` re-encodes it with the same codec (`zbz_zcl.h`) before queuing.
3. The panel's endpoint must carry the IAS WD client and IAS ACE server before
   `esp_zigbee_start`; whether adding them changes the stack's stored network is a bench check.

A fob's source, once open here, is decided in §3.
