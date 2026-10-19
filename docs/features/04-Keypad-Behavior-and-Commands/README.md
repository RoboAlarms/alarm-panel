# 04 · Keypad behaviour and commands

**Status:** Implemented v1 - The key parser (`alarm_keyseq`, host-tested, used by the console's
`keys`), the touch keypad with the panic hold and the keypad messages run on a panel and in the PC
preview, with the lockout countdown and a test of Home's words (`ui_words_test`); the flows'
acceptance on a panel is BL-024. Remote keypads over Wi-Fi run with the panel link
(docs/features/11, tried on two panels 2026-10-16). Partition switching (KPD-006, BL-048) is implemented with per-panel policy, no-code selection and
interaction-based idle return, host/preview tested; live acceptance remains. A classic two-line
view is BL-073.
· **Milestone:** M1 (parser), M3 (UI), M7 (remote keypads) · **Depends on:** 01, 07, 08, 11

## Goal
Every keypad (the master's own screen, a remote CrowPanel, a CYD) behaves like a classic alarm
keypad: the same key sequences, messages, sounds and panic keys, while the touch UI adds
friendlier buttons for the same commands.

## Reference behaviour
Classic alpha keypads accept `code + function key`: OFF (1), AWAY (2), STAY (3), MAX (4),
TEST (5), BYPASS (6), INSTANT (7), CODE (8), CHIME (9), READY/[*] (show faults), and `#` quick
functions. Panic keys are key pairs. They show two-line messages ("DISARMED READY TO ARM",
"ALARM 03 ..."), scroll through faults, and beep. Touchscreen keypads add touch buttons (Arm Away /
Home / Night, No Entry Delay, Silent Exit, Bypass, Quick Exit, panic icons).

## Key sequences (implemented in `alarm_keyseq`)

Codes are **4 to 10 digits, chosen per user** (like an iPhone passcode). A classic keypad knows
a code has ended after 4 digits, so there the next digit is the command (`code + 2` = AWAY).
With variable lengths that's ambiguous, so the commands are **dedicated keys** with the
classic labels: type the code, then press **AWAY**, **STAY**, **OFF** and so on. The `#`
quick functions keep their digits because `#` can't start a code.

| Keys | Action | Touch UI equivalent |
|---|---|---|
| code + OFF | Disarm, else silence, else end walk test, else clear memory + acknowledge troubles | Disarm |
| code + AWAY | Arm Away | Arm Away |
| code + STAY | Arm Stay | Arm Stay / Home |
| code + NIGHT | Arm Night (Night-Stay) | Arm Night |
| code + MAX | MAXIMUM (away, no entry delay; exit time with beeps) | Arm Away + "Instant" |
| code + INSTANT | INSTANT (stay, no entry delay; quiet exit time; every mode keeps the exit time) | Arm Stay + "Instant" |
| code + TEST | Walk test on/off | Menu -> Walk test |
| code + BYPASS + zzz zzz ... + `#` | Bypass zones (3 digits each with > 99 zones; `#` or `*` finishes) | Zones -> Bypass |
| code + BYPASS + `#` | Quick bypass (all faulted zones) | "Bypass all & arm" |
| code + CODE + uu + new code + `#` | Add/change user uu (new code 4-10 digits; only while disarmed) | Users |
| code + CODE + uu + `#` + 0 | Delete user uu (only while disarmed) | Users |
| code + CHIME | Chime on/off (only while disarmed) | Chime toggle |
| CHIME alone | Chime on/off without a code, when the installer turned "chime needs a code" off ([01](../01-Core-Alarm-Engine/README.md) CORE-CHIME-005); refused with it on, like a lone arming key without quick arm | rail > Chime, one touch |
| code + `*` + p | GOTO partition p (keypad-local) | Partition selector |
| OFF alone | Silence fire/CO/aux alarms and trouble beeps (no code) | "Silence" |
| AWAY / STAY / NIGHT alone, `#` 2 / `#` 3 | Quick arm (only if enabled) | Arm buttons without a code |
| `#` 9 | Quick exit (if enabled) | "Quick Exit" while armed Stay |
| `*` | Show faults (disarmed) / exit restart (armed Stay/Night) | Fault list / Quick Exit |
| A / B / C | Panic keys (default police / medical / fire) | Panic buttons, **held 2 s** |

An unfinished sequence is dropped after 10 s. A panic key always works, discarding a
half-typed code. While a code is being typed the UI shows one dot per digit
(`alarm_keyseq_digits()`), never the digits themselves. Codes are not auto-submitted when
"enough" digits are entered, because different users have different lengths; the command
key is the submit.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| KPD-001 | Key sequences as above | `CORE-KEY-001..014` |
| KPD-002 | Touch UI and classic keys produce identical engine commands: the key parser and the touch UI both build `alarm_cmd_t` for `alarm_engine_command()` | `CORE-KEY-*`, UI preview |
| KPD-003 | Panic buttons need a 2 s hold with visual progress; a silent panic or a refused command confirms with a toast, an audible panic needs none because the screen becomes the alarm (as built) | UI preview |
| KPD-004 | Keypads show the classic message set (below) for every status | UI preview, ui_words_test |
| KPD-005 | Keypads play the partition's continuous pattern and one-shot sounds ([08](../08-Audio-and-Annunciation/README.md)) | M3 |
| KPD-006 | Each panel has an installer-set default partition, permission to switch or remain locked there, and an installer-set idle return delay (default 30 s). Selecting another enabled partition needs no code; alarm actions keep their normal authorization | M7 |
| KPD-007 | A remote keypad that loses the master shows "No system communication" after 30 s and disables arming controls; panic still attempts to send | M7 |
| KPD-008 | The keypad shows a lockout countdown when its source is locked out | `test_users.c`, UI preview |

## Messages (keypad model)

| Condition | Classic alpha display | Our display (title / detail) |
|---|---|---|
| Disarmed, ready | DISARMED READY TO ARM | **Ready to arm** |
| Disarmed, not ready | DISARMED Hit * for faults | **Not ready** / fault list |
| Fault | FAULT 03 LIVING WINDOW | "Living Window open" |
| Bypassed zone | BYPAS 03 ... | "Bypassed: Living Window" |
| Exit time | ARMED AWAY/STAY You may exit now | **Arming Away** / Stay / Night, the seconds in the ring ("Quiet exit time" for Stay and Night; "No entry delay" in the note for Instant); the Disarm pad beside it at once ("Cancel arming") |
| Armed | ARMED \*\*AWAY\*\* / STAY / NIGHT-STAY / INSTANT / MAXIMUM | **Armed Away** / Stay / Night, "No entry delay" in the note when armed Instant (INSTANT / MAXIMUM are called "Stay Instant" / "Away Instant" on the pad and in the event sentences); the Disarm pad beside it |
| Entry time | DISARM SYSTEM OR ALARM WILL OCCUR | **Disarm now** / countdown |
| Alarm | ALARM 03 LIVING WINDOW | **ALARM** / zone list (red) |
| Fire | FIRE 06 HALL SMOKE | **FIRE** / zone (red, temporal-3) |
| CO | CO ALARM 08 | **Carbon monoxide** |
| Alarm memory after disarm | ALARM 03 (until code + OFF) | "Alarm occurred: ..." (acknowledge) |
| Exit error disarmed | CANCELED ALARM / CA | "Alarm canceled - door left open" |
| Exit alarm | EXIT ALARM / EA | "Exit alarm - door left open" |
| Zone trouble | CHECK 03 | "Living Window · Not reporting" / "Check the sensor" |
| AC loss / low battery | AC LOSS / LO BAT | "Mains power lost" / "Running on battery"; "Panel battery low" |
| Comm failure | COMM. FAILURE / FC | "Reports not getting through" / "A connected service can't be reached" |
| Link lost (keypad) | - | "No system communication" |

The third column gives the gist; the exact words are in `ui_app.c` (Home's headline, detail and
note) and `components/core/notify/src/notify_words.c` (events and troubles, guarded by
`test_notify.c`: test_golden_wording, test_trouble_words).

The keypad model turns the status snapshot, zone statuses and names into these texts, so every
keypad renders identical words. As built (M3) it is part of `components/ui/alarm_ui`: `ui_app.c`
derives the state and Home's headline, detail and note; the event and trouble sentences have
their single copy in `components/core/notify/src/notify_words.c`
([26](../26-Notifications/README.md)), and `ui_text.c` keeps the tones, the icon table, the zone
card and the words only this screen uses (authority details, zone types, device classes). It is
portable C that runs on the panel and in the PC preview, and will run on a remote keypad (M7)
([07](../07-UI-LVGL-Pro/README.md)). The touch UI's wording is friendlier than the alpha
display: "Disarmed / Ready to arm", "Not ready / Living Room Window is open", "Arming Away /
Leave now through the exit door", "Disarm now / Entry through Front Door", "Burglary alarm /
Front Door", "Alarm occurred", "Carbon monoxide".

## Design
- **Keypad model** (`ui_app.c`) holds the last status snapshot and the zone names and computes
  the subjects the screens bind to.
- **Commands** go through `ui_backend_t` (the keypad client): `alarm_service` on the master,
  the panel link on remote keypads (M7). The touch UI builds `alarm_cmd_t` itself because some
  options (bypass-and-arm, silent exit on Night) have no key sequence; the classic view (not
  built yet, BL-073) and the serial console (`keys`) use `alarm_keyseq`.
- **Show faults**: `*` or the fault list shows zones with FAULT/TROUBLE/BYPASS flags, alarms first.

## Test and acceptance
- Host: `components/core/alarm_core/test/test_keyseq.c` (one test per `CORE-KEY-001..014`);
  the event sentences in `components/core/notify/test/test_notify.c`.
- PC preview (`pwsh tools/ui_preview.ps1`): the touch keypad, the panic hold and the keypad
  messages (KPD-002..004).
- On a panel: the arming flows and the panic hold on the touchscreen (BL-024).

## Open questions
- Offer a scrambled keypad layout (anti-smudge) as an option? Decided: no - the
  keys stay where hands expect them.
- Should a lone `*` on a disarmed classic view also chime the fault list (a classic keypad scrolls
  silently)? It belongs to the classic view (BL-073).

## Tasks
- [x] Keystroke parser + tests
- [x] Keypad model and messages (M3, `components/ui/alarm_ui/src`)
- [x] Touch keypad + panic hold widget (M3)
- [x] Host tests for the event sentences, now in `components/core/notify` (`test_notify.c`
      test_golden_wording, test_every_event_type_has_words)
- [x] Host tests for Home's words and ui_text.c (ui_words_test) (BL-066, 5153415)
- [ ] Classic two-line keypad view, optional, driven by `alarm_keyseq` like the console (BL-073)
- Remote keypads: KPD-007 ("No system communication", Panic still sent) and KPD-008 (each
  keypad's own lockout) are built with the panel link (docs/features/11 LINK-008, LINK-023);
  KPD-006 (GOTO) is docs/features/01's box (BL-048).
- Trying the arming flows and the panic hold on a panel is part of BL-024 (the touchscreen on a
  panel), whose boxes are in docs/features/07 Tasks.
