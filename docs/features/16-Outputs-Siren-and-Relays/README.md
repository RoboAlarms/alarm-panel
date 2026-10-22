# 16 · Outputs: siren, strobe and relays

**Status:** In progress - every kind of siren follows the engine's bell through one policy
(`components/core/sirens`, host-tested): wired outputs on the board's terminals (siren with
temporal-3/4, strobe, smoke detector power, armed light; chirps; bell supervision), 433 MHz sirens
the panel transmits to, Z-Wave sirens (`zwave_zones/zw_siren`), and Zigbee's (docs/features/20);
every kind of siren may also sound for other partitions' alarms (OUT-015). The touchscreen pages
were approved by the owner on 2026-10-20 (the mockup's `screens/sirens.js`), built and tried in the
PC preview; nothing of it has been on a panel or a real siren yet (BL-037). Left: hardware checks,
the Panel Manager's pages, programmable rules
· **Milestone:** M1 (core), later (hardware, rules) · **Depends on:** 01, 02, 17, 20, 25, 36

## Goal
Drive the physical siren and strobe from the engine's outputs, whatever carries them, and offer
programmable outputs (for example "turn on the porch light during entry delay").

## Reference behaviour (research 2026-10-20: DSC PowerSeries Neo, Resideo VISTA-21iP, Qolsys IQ Panel 4)
- **Bell cutoff:** 4 minutes by default on all three (DSC [005] 1-255 min; VISTA \*33 4/8/12/16
  min; Qolsys 3-15 min). Fire may sound until silenced (DSC [014] opt 8, VISTA \*32; UL fire
  needs it). The engine has both (`bell_timeout_s` 240, `fire_bell_timeout` off).
- **Patterns:** steady for burglary and panic, temporal-3 for fire (NFPA 72), temporal-4 for CO
  (NFPA 720). Priority fire > CO > burglary (DSC's bell cadence, Qolsys's signal order).
- **Squawk:** 1 on arming, 2 on disarming (DSC [014] opt 1; VISTA \*38 is one "ding"), off by
  default. DSC also limits it to keyfob users or Away; not built here.
- **Swinger shutdown:** a zone past its trip count no longer sounds the bell (DSC [377]): the
  engine already ends there (`eng_alarm`).
- **Wireless sirens** are assigned to partitions with per-alarm-type switches (DSC PG9901
  [804][551]: partitions, Fire/CO/Burglary/Flood/Temp, strobe, squawk, volume) and supervised far
  more tightly than sensors (DSC: 13 minutes; Z-Wave sirens on Qolsys: 4 h). Qolsys sounds a Z-Wave
  siren for every audible alarm.
- **Bell supervision:** DSC supervises the bell output always (1 kΩ dummy load when none is
  fitted); VISTA \*91 with an EOL at the last sounder. Here: an optional input wired to the siren's
  fault contact or a supervision loop.
- **Programmable outputs:** DSC PGM types (101 fire and burglary follower, 103 sensor reset, 114
  ready, 115-117 armed status, 121-124 command outputs, 156 latched strobe); VISTA \*79/\*80 maps
  relays to zones, zone types or events with on, off, pulse, toggle.
None of the three distinguishes interior and exterior sirens: a siren's partitions and alarm
types say which sounds. None offers a siren for another partition's alarm as a setting of its
own; DSC's shared bell sounds for every partition it is assigned to - here that is a siren's
partitions, and "Also sounds for" adds alarms without the chirps (OUT-015).

## Engine outputs (implemented)

| Output | Scope | Patterns | Behaviour |
|---|---|---|---|
| `BELL` | partition | `steady` (burglary, audible panic, tamper, 24 h audible), `temporal3` (fire), `temporal4` (CO), `test` (1 s pulse at walk-test start), `off` | Burglary times out after the bell timeout; fire/CO sound until silenced (default) |
| `STROBE` | partition | `on` / `off` | On with any audible alarm, stays on until disarm |
| `SMOKE_RESET` | system | `on` / `off` | Interrupts 4-wire smoke detector power: fire memory clear, fire verification |

Keypads mirror the alarm on their own speakers ([08](../08-Audio-and-Annunciation/README.md)).

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| OUT-001 | Engine output patterns as above | `CORE-ALM-001, 012`, `CORE-FIRE-*`, `CORE-TEST-001` |
| OUT-002 | Wired outputs on the board's terminals driving relay/MOSFET modules; temporal-3 and temporal-4 on the line exact to the millisecond, from the outputs' own task at its deadlines, not the UI | `test_sirens.c` (`test_temporal_patterns`, `test_wired_fire_cadence`); HIL |
| OUT-003 | Several partitions share one siren; the most urgent sound wins (fire, CO, burglary, then the bell test); a siren sounds for burglary and panic, for fire and CO, or both | `test_fire_wins_across_partitions`, `test_scope_by_alarm_kind` |
| OUT-004 | Optional bell supervision input (the siren's fault contact or a loop, to ground): open 10 s is the bell trouble; closed clears it | `test_bell_supervision`; HIL |
| OUT-005 | Programmable output rules: trigger (event type, partition, zone list, arm state, trouble) -> action (on, off, pulse N s, follow) with at least 8 rules | On, off and on for a time: docs/features/41's automations (BL-224, `AUTO_ACT_OUTPUT` on an output "For automations", OUT-023, OUT-024; `test_automation_output_ignores_the_engine`); pulse and follow later |
| OUT-006 | Outputs start in a safe state: a channel's pin is an output at its inactive level the moment it is configured (before the engine says anything); a relay board that switches on low is driven high | `test_safe_at_boot`; preview (`bl-037-wired-end`); HIL |
| OUT-007 | Squawk: one chirp on arming, two on disarming, for sirens with `squawk` (off by default); never while it sounds (the alarm's end is the feedback) | `test_chirp_counts`, `test_wired_squawk`, `test_rf_squawk`, `test_zw_siren.c` |
| OUT-008 | The bell test (walk test start) is a one-shot pulse of 1 s, never over a sounding alarm | `test_bell_test_is_a_pulse` |
| OUT-009 | A wired channel is a siren, strobe, smoke detector power or armed light; no two share a pin; "Test it" sounds 2 s, refused while armed; stored as NVS `outputs`/`wired` (frozen fixture) | `test_other_functions`, `test_wired_configuration_rules`, `test_wired_test`, `test_wired_store`; preview |
| OUT-010 | A 433 MHz siren gets the alarm code at once and every 20 s while its sound is wanted, the stop code 3 times when it ends; fire and CO sound its one sound | `test_rf_alarm_repeats_and_stops`, `test_rf_test` |
| OUT-011 | Pairing a 433 MHz siren sends its pairing code (the arm code) 3 longer times; refused while armed | `test_rf_pairing`; preview (`bl-037-rf-siren-pair`) |
| OUT-012 | A 433 MHz siren can't be supervised: its pages say so; nothing able to transmit while one is assigned is the bell trouble | `test_rf_trouble_without_transmitter`; preview |
| OUT-013 | The 433 MHz sirens are stored as NVS `outputs`/`rf` (frozen fixture), all or nothing | `test_rf_store` |
| OUT-014 | A new 433 MHz siren's address is 20 bits, never a factory-shared one (RF-018), never another siren's | `test_rf_addresses` |
| OUT-015 | A siren also sounds for other partitions' alarms (and their strobe) when chosen ("Also sounds for": the house's siren for the shop's alarm) - never their chirps, their bell test or their armed state; on every kind of siren (wired, 433 MHz, Z-Wave, Zigbee) | `test_also_sounds_for_another_partition`; `test_zw_siren.c`; `test_zbz_wd.c` (`test_wd_also_sounds_for`, the frozen `wd_fixture_v2.h`); preview (`bl-037-wired`, `bl-147-siren-end`) |
| OUT-016 | Z-Wave sirens follow the bell: Sound Switch tones per alarm kind (burglary, fire, CO) at its volume, or Binary Switch on/off; stop when the bell ends | `test_zw_siren.c`, `test_zwz_panel.c` `test_siren_follows_the_bell` |
| OUT-017 | A Z-Wave siren's Set not delivered is sent again, 3 failures are the bell trouble; while it sounds it is asked every 60 s and sent again if it went quiet | `test_zw_siren.c` |
| OUT-018 | A Z-Wave siren not heard for its limit (4 h by default, at least 1 h) while the radio works is the bell trouble | `test_zw_siren.c` |
| OUT-019 | Z-Wave sirens are stored as NVS `zwave`/`siren` (frozen fixture); squawks and test refusals as the others | `test_zw_siren.c` |
| OUT-020 | The bell trouble stands while any kind of siren holds it (Zigbee, Z-Wave, wired, 433 MHz): one clearing its own never clears another's | `alarm_service_bell_trouble` (code review; HIL) |
| OUT-021 | A terminal an output takes is no zone's: refused while a zone uses it; never offered, learned or bound while the output has it; given back when the output goes | `test_zone_hub.c` `test_wired_terminal_reserved_for_an_output`; preview (`bl-037-wired-add`) |
| OUT-022 | The panel transmits on 433 MHz: the CC1101 between receptions, or a transmitter module's DATA pin (`rf txpin`); `rf send` is the bench's test remote | HIL (`rf send`) |
| OUT-023 | A wired channel may be "For automations" (BL-224): a name typed like a zone's (up to 23 bytes), its terminal and relay board, no partitions, no supervision; "Test it" switches it on 2 s, armed or not; stored apart in NVS `outputs`/`auto` (its own format and CRC, the frozen `sirens_fixture_auto_v1.h`), never in `outputs`/`wired`, which stays as before so older firmware reads it whole; `wired` skips an entry of a function it doesn't keep there | `test_automation_output_configuration`, `test_automation_output_test`, `test_automation_output_store`; preview (`bl-224-output-add`) |
| OUT-024 | An output for automations is on only while an automation sets it (honouring the relay board), off at boot and when configured again (OUT-006), and never follows the engine: no alarm, strobe, smoke reset, armed state or chirp (sirens stay outside automations, AUT-002) | `test_automation_output_ignores_the_engine`, `test_automation_output_configuration` |

## Design
```
engine on_output (alarm task) --panel_app--> outputs_service (queue, own task)  -> out_wired (pins), rf_siren (rf_service_transmit)
                                         \-> zwave_service (posted)             -> zwz_panel -> zw_siren (Sound/Binary Switch)
                                         \-> zigbee_service (posted)            -> zbz_panel -> zbz_wd (IAS WD)
engine snapshot (armed per partition) -------> the same three, for squawks
each siren kind's trouble ------------------> alarm_service_bell_trouble(owner) -> ALARM_TRBL_BELL
```
- **One policy** (`components/core/sirens/siren.h`): the engine's outputs per partition, and a
  scope per siren (own partitions, `also`, burglary, fire); `siren_pick` gives the sound. The
  wired driver plays it on a line; the 433 MHz driver sends codes; Z-Wave sends tones.
- **Wired outputs** (`out_wired`, `outputs_service`): 4 channels on any free terminal, driven
  through a relay or MOSFET module (never a siren off a pin: a 12 V siren has its own supply).
  `active_low` for the common relay boards. The task runs at priority 8 and wakes at the next
  pattern edge.
- **433 MHz sirens** (`rf_siren`, `rf_service`): the cheap sirens sold with 433 MHz kits learn a
  "remote" and sound, stop and chirp on its codes. The panel is that remote: an EV1527 address of
  its own per siren, the common four-button fob's codes by default (alarm 8, stop/disarm 1, arm 2;
  changeable with `out rf <n> codes`). The CC1101 transmits between receptions (`cc1101_tx_ook`,
  PATABLE +10 dBm at 433 MHz, the EU SRD limit) on its GDO0 line, driven by the RMT (`rf_tx.c`);
  a receiver module can't transmit, so a WL102-341/FS1000A on a terminal (`rf txpin`) does.
- **Z-Wave sirens**: docs/features/36 [sirens.md](../36-Z-Wave-Sensors-ZAC93/sirens.md).
- **Zigbee sirens**: docs/features/20 [alarm-devices.md](../20-ESP32-H2-Sensor-Radio/alarm-devices.md)
  §2 (built before this: partitions, burglary/fire, strobe, squawk, volume, supervision). Its
  partitions list is its own scope; "Also sounds for" is one byte more in its stored entry (`also`,
  after the supervision time: older firmware skips it as a later field, the frozen
  `wd_fixture_v2.h`), and a Panel Manager page that doesn't send it keeps what the siren has.
- **Screens** (approved 2026-10-20; the mockup's `screens/sirens.js`, `ui_sirens.c`):
  Installer > Sirens and outputs (every siren), a wired output's page, a 433 MHz siren's page and
  its pairing, Installer > 433 MHz (receiver, transmitter, sensors, sirens), and the siren rows of a
  Z-Wave siren's device page (`ui_zwave.c`).
- **Console:** `out` (wired and 433 MHz; `out wired <n> auto <gpio> [low] [name <words>]`, `out
  wired <n> on|off` for an output for automations), `rf send`, `rf txpin`, `zwave siren`.
- **Outputs for automations** (BL-224, OUT-023, OUT-024): the function `OW_FN_AUTOMATION` (5),
  switched only by `ow_auto_set` - the automations' `AUTO_ACT_OUTPUT` through
  `outputs_service_auto_set`; `outputs_service_auto_list` and `outputs_service_auto_mask` tell the
  automations which outputs they may name (docs/features/41 DESIGN.md section 15). They are stored
  in a key of their own, `outputs`/`auto`, so a downgrade never costs the sirens: firmware from
  before BL-224 reads `outputs`/`wired` whole and ignores `auto`, so an output for automations is
  simply gone there and its terminal shows as free (back after upgrading again, unless the terminal
  was given to something else meanwhile).
- Programmable rules: the panel's one rule table is docs/features/41's automations (ADR-049). On,
  off and on for a time are built (BL-224); pulse and follow stay output functions (later).

## Wiring a wired siren (unverified on a panel)
J7 terminal -> the IN pin of a 3.3 V-logic relay or MOSFET module (its VCC from the module's own
supply as its maker says; GND shared); the siren on the relay's contacts with its own 12 V supply.
Most relay boards switch on low: "Relay board: Switches on low". A supervision input: another
terminal to the siren's fault/tamper contact, closed to GND while healthy. The terminal numbers
are the board's (`terminals` on the console); GPIO 49-52 on the 7" J7 header.

## Open questions
- The 433 MHz sirens' codes: the defaults are the common fob's; each siren model is to be tried on
  the bench (`rf send <addr> <code>` while it learns) and its codes written down in docs/sensors.
- Should a wired siren follow "Bell delay" (DSC [005])? Not built: the engine has no bell delay.
- Should keypads (panel link) drive local outputs? Not built.

## Tasks
- [x] Engine outputs + tests (834e0ad)
- [ ] Output service + drivers: wired outputs (OUT-002..OUT-006, OUT-009), 433 MHz sirens
  (OUT-010..OUT-014, OUT-022), Z-Wave sirens (OUT-016..OUT-019), the shared policy and "Also sounds
  for" (OUT-003, OUT-015), the bell trouble's owners (OUT-020), terminals kept apart (OUT-021) -
  built and host-tested on branch bl-037-sirens-wired-rf, not on a panel (BL-037)
- [ ] On a panel: a relay board on a J7 terminal with a siren, its patterns, the supervision input;
  a 433 MHz siren paired and sounding from the CC1101 and from a transmitter module; a Z-Wave siren
  (Zooz ZSE19/ZSE50) (BL-037)
- [ ] The Panel Manager's pages for sirens and outputs (WEB-026: a proposal beside today's pages
  first); the touchscreen's were approved on 2026-10-20 (BL-037)
- [ ] Programmable output rules (OUT-005): a relay as an action of docs/features/41's automations
  (16 of them, ADR-049), with pulse and follow as output functions, and their host tests (later)
  (BL-037)
