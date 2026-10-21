# 25 · RF sensors: 315, 345 and 433 MHz

**Status:** In progress - 433 MHz through a CC1101 or a fixed receiver module: five protocol
families, learning, confirmed frames, check-ins and jamming, host-tested; a module on a panel
and a CC1101 on air. The sensors people buy are tested as mock devices (21 products and 4
neighbours, "Mock devices"), which found B025 (a slow sensor's sync outlasted the receiver's
idle; fixed in code, not yet on a panel, BL-163). A receiver on your own pins (RF-025, ADR-048) is
built and host-tested, not yet on a panel (BL-165). Left: bench fixtures, the low-battery code
and B012/B014 with a real contact (BL-022); decoders for the 345 MHz, legacy 433 MHz and 319.5 MHz security sensors (BL-039).
· **Milestone:** M14 · **Depends on:** 02, 15, 17, 22 ·
**Hardware:** a CC1101 module on SPI plus one data pin, or a receiver module on the data pin
alone (the CrowPanel's wireless socket pads by default)

## Goal
Use the cheap one-way sensors sold everywhere (door contacts, PIRs, water and smoke detectors
on 433.92 MHz with EV1527 / PT2262 chips) and, with the right receiver, the sensors people
already own on 345 MHz, 319.5 MHz and legacy 433 MHz security systems. This is the zone
source (docs/features/22) that most directly breaks the "one radio family per panel" rule.

## What's out there (research, September 2026)
- **EV1527**: 24-bit code word, 20-bit fixed address + 4 data bits, not encrypted
  ([espboards](https://www.espboards.dev/blog/esp32-433-rmt-ev1527/)). Door sensors send one
  data value for open, and many send others for closed, tamper or low battery (it varies by
  vendor), so the panel learns each code. Most send no periodic heartbeat. **PT2262 / PT2264 /
  SC2260** remotes and sensors put tri-state symbols on the same frame: physically identical,
  so one decoder serves both.
- **345 MHz security sensors**: OOK, ~3.7 kbit/s Manchester, 64-bit packets: 4-bit channel,
  20-bit serial, event byte with contact, reed, alarm, tamper, battery and **heartbeat** bits,
  CRC-16 (rtl_433 has a decoder for it). The heartbeat allows real supervision. A second 345 MHz
  family uses the same format with another CRC polynomial.
- **ESP32 + CC1101** projects already decode these: [OpenRF-Sentinel](https://github.com/OneBigHen/OpenRF-Sentinel)
  (345 MHz security sensors and EV1527 / PT2262 at 433 MHz), [rtl_433_ESP](https://github.com/NorthernMan54/rtl_433_ESP)
  (a port of rtl_433's decoders for OpenMQTTGateway). Their CC1101 settings were cross-checked
  against TI's datasheet and DN022 for the register table in `cc1101.c` (2026-09-22): the
  points that matter are asynchronous serial output on GDO0 (IOCFG0 0x0D, PKTCTRL0 0x32), no
  sync word (MDMCFG2 0x30), a data rate kept near 4.8 kBaud because the demodulator still
  samples at 8x it, a 325 kHz filter, TI's OOK AGC values (0x07 / 0x00 / 0x91), and a GDO0 pin
  that is not a strapping pin (the line idles high on noise).
- **What Home Assistant uses (research 2026-10-04)**, the places these sensors' behaviour is
  written down: **rtl_433** (an SDR, decoders for 250+ devices, to MQTT with autodiscovery) and
  **OpenMQTTGateway** with **rtl_433_ESP** (rtl_433's decoders on an ESP32 with a CC1101) carry
  the alarm sensors' formats - cheap contacts, smoke alarms and water sensors, the generic PIR
  (`generic_motion.c`), the 345 MHz and 319.5 MHz security sensors - plus "flex" configs people
  wrote for single products (EV1527 contacts and PIRs, a water sensor, Sonoff RM433, a driveway
  alarm). **RFXtrx**
  takes EV1527/PT2262 as "Lighting4" (a device with an open and a closed code is two devices with
  `command_on`/`command_off`, and `off_delay` for open-only sensors - our hold, RF-022); **RFLink**,
  **ESPHome**'s `remote_receiver` (rc-switch protocols) and **Tasmota** on a Sonoff RF Bridge
  (Portisch firmware: `RfReceived` with Sync, Low, High and Data) are the rest. Their issues and
  captures are where products' codes and timings come from ("Mock devices" names the source of
  each).
| ID | Requirement | Verified by |
|---|---|---|
| RF-001 | Receiver options: a **CC1101** on SPI (tunable 300-348 / 387-464 MHz, RSSI for jamming detection) or a fixed superheterodyne receiver (RX470-4 / WL101-341, RXB6, SRX882) on one GPIO; one or two receivers at once | `rf_service.c`: `auto` (CC1101 if one answers, else a module found by the static on its line), `cc1101`, `ook`; HIL pending. Two at once: later |
| RF-002 | Pulse capture with the RMT peripheral; decoding in a portable `rf_decode` component (EV1527 and PT2262 first, then the 345 MHz and 319.5 MHz security-sensor protocols) tested on the host with recorded pulse traces | `rf_capture.c`; `test_rf_decode.c` (44 tests on encoder-made frames sent through a model of the air and the receiver, RF-013) and every capture in `test/rf/` (`test_recorded_captures`; two synthetic ones until the bench records real sensors) |
| RF-003 | Learn by triggering: "open the door now", "close it", "press tamper"; each learned code is stored in the zone's binding (docs/features/22) | `test_rf.c` (learning, candidates newest first, the table making room), preview `113_rf_learn` .. `115_rf_open` |
| RF-004 | Sensors with heartbeats (the 345 MHz security sensors) are supervised: CHECK when no heartbeat arrives within the zone's supervision time (defaults from docs/features/02: 12 h burglary, 4 h life safety); sensors without heartbeats are marked unsupervised and the zone editor says so | `test_rf.c` (no `ZSRC_CAP_SUPERVISED`; the zone page's trust facts say "not supervised"); 345 MHz: later |
| RF-005 | Jamming: a carrier above the noise floor (CC1101 RSSI, or a module's line held high) for 30 s in all of the last minute - however it is chopped up (the EN "30/60" rule; a detector that resets on any gap is beaten by 50 s on / 0.2 s off) - raises the `sensor_radio` trouble; it is over once the last minute holds under 5 s. A sample within 500 ms of a decoded frame (`ZSRC_RF_TALK_MS`) is the sensor talking, not a carrier - a contact a metre from the panel reads -15 to -27 dBm and talks for two seconds an event, and a CC1101's line is never quiet enough to sample between its frames (B012) - unless its transmission has gone past 100 frames: a stuck transmitter still counts | `test_rf.c` (30 s takes the link down; the hub's trouble after its 60 s grace; `test_a_pulsed_jammer_is_still_jamming`; `test_a_busy_band_is_not_jamming`; a short carrier is nothing; `test_a_sensor_talking_is_not_jamming`, `test_a_stuck_transmitter_is_jamming`, `test_a_jammer_with_a_sensor_in_between_is_jamming`); threshold to be settled on air (noise floor on a panel: -83 dBm) |
| RF-006 | Fixed-code sensors can be replayed by anyone with a transmitter: the zone editor states it, and life-safety types need the installer option (ZSRC-008). Replayed "closed" codes can't hide an intrusion that sends "open" first, but a replayed "open" can cause false alarms: repeated identical packets are counted, and a burst far above the sensor's normal repeat pattern is logged | `test_rf.c` (no `ZSRC_CAP_AUTH`, not trusted for fire/CO/panic; transmissions over 100 frames counted as bursts); the zone page's trust facts |
| RF-007 | A keypad (M7) with a receiver can act as an RF zone expander for the master | HIL (M7+) |
| RF-008 | The receiver's data line can move to any pin the board offers outside (its own data pin or a wired zone terminal), per panel: `rf pin <gpio>` / `rf pin default`, stored in NVS (`rf`/`pin`) and used from the next start. The app takes that terminal out of the wired terminals before the zone sources start, so the zone editor never offers it; a pin a zone uses, a pin the board does not offer, or a wiring the board validator refuses is turned down with the reason | `board_rf_data_on()` in `components/core/board_info/test` (BRD-013) and the 7"'s `test_board_desc.c` (every J7 terminal, refusals); the Flasher's `test_flasher_panel.py`; on the second panel (DATA on J7 IO3) |
| RF-009 | A transmission counts only once its frame has arrived twice (`ZSRC_RF_CONFIRM_FRAMES`): EV1527 has no checksum, so one frame alone may be noise or a copy with a bit flipped, and would look like a new sensor. Several transmissions are followed at once, so a stray frame between a transmitter's repeats does not split its transmission. Only confirmed transmissions are learned, logged by `rf raw on` and counted as `confirmed=`; since RF-014 a bound code's zone waits for the confirmation too | `test_rf.c` (`test_a_lone_frame_is_not_a_sensor`: a lone frame is never offered, a corrupted repeat does not split the real transmission); preview `113_rf_learn` (a corrupted frame is sent and not offered) |
| RF-010 | The learn list has one row per sensor with the codes it sent ("Codes 5 and E, heard 2 times"), not one row per code; the code it sent first while learning is the one bound (the door is opened first; a zone's tamper and low-battery codes are picked per code instead, RF-021). Sensors already transmitting in the ten minutes before learning began are listed apart, under "Already transmitting before you started" | `test_rf.c` (`test_learning_lists_sensors_not_codes`, `test_the_learn_table_makes_room_for_the_newest`); preview `113_rf_learn` (a neighbour's remote set apart) |
| RF-011 | Once one sensor has answered the installer at least twice while learning, more often than any other, the picker names it ("Found it: Sensor 89288 answered each time") with one **Use Sensor 89288** button and a beep; a tie names nobody and the list stays to choose from | `test_rf.c` (`test_the_best_sensor_is_the_one_that_answered`); preview `113_rf_learn` |
| RF-012 | Every protocol family in "Protocols" below is decoded from the same pulses at once, each by its own rules, and none reads another's frames; a packet names its protocol, the transmitter (`device`) and what it says (`data`), with its flags (battery low, rolling code, PT2262 shape). The numbers are stored in bindings: never renumbered, `ZSRC_RF_PROTO_*` equal to `rf_proto_t` | `test_rf_decode.c` (`test_every_protocol_round_trips`, `test_protocols_do_not_read_each_other`, `test_the_protocol_table_is_stable`, per-protocol field tests); `test_rf.c` (`test_the_numbers_are_the_decoders`, `test_every_protocol_binds_by_its_own_widths`) |
| RF-013 | The decoder loses frames, never makes codes: pulses shorter than 60 us are healed into their neighbours; the unit comes from the whole sync pair; a bit is the template its pair is nearest to by a clear margin and as long as a real bit; a frame is exactly its bit count between syncs (a lost or extra pair breaks it, never shifts it); a frame whose marks, spaces and bit lengths of one kind vary more than one transmitter's do is refused (collisions, noise) | `test_rf_decode.c` (slicer, jitter, AGC settling, glitches, lost and extra pairs at every position, collisions, static for ~17 minutes, carrier, inverted line, foreign Manchester signals); the one limit kept in writing: two transmitters with the same clock, aligned (`test_two_aligned_transmitters_can_make_a_third_code`) |
| RF-014 | A zone moves only on a confirmed transmission: a frame without a checksum must arrive twice (as a PT2272 decoder waits for two code words); a lone frame - a flipped bit, a collision - moves nothing, opens nothing, restores nothing. A protocol with a checksum acts on its first frame | `test_rf.c` (`test_a_lone_frame_moves_no_zone`, `test_two_sensors_interleaved_both_count`, `test_a_checked_frame_acts_at_once`, `test_a_door_opened_and_shut_at_once`) |
| RF-015 | Every confirmed transmission of a bound sensor, whatever its code, is a check-in for its zone (at most one a minute), so the zone's supervision time works for sensors that report now and then | `test_rf.c` (`test_every_transmission_is_a_check_in`) |
| RF-016 | A rolling-code transmitter (KeeLoq) is bound by its serial number and button; a hopping code heard in an earlier transmission is a replay and is refused (counted as `replays`); its battery bit reaches the zone as low battery and its restore | `test_rf.c` (`test_a_replayed_rolling_code_is_refused`, `test_the_battery_bit_reaches_the_zone`) |
| RF-017 | The CC1101: genuine, clone or nothing told apart; every register of the receive table read back after writing (a wiring fault is named at once); a chip that reset under the panel (IOCFG0 back at its 135 kHz clock) is configured again within 5 s; recalibrated every hour; RSSI read until two reads agree (errata SWRZ020) | `components/core/cc1101_regs/test` (table, frequency words against TI's formula, verify, identify, reset detection, RSSI); the driver on the bench |
| RF-018 | An address many transmitters share from the factory (all 0, all 1, a PT2262 with every address pin open) is marked: `rf raw on` and the Flasher say "common factory address" | `test_rf_decode.c` (`test_common_factory_addresses`); `test_flasher_panel.py` |
| RF-019 | **The receiver at another place the board offers**: `rf place <place>|default`, stored in NVS (`rf`/`place`) and used from the next start. The CrowPanel offers `j7` for a CC1101 (SCK IO52, MOSI IO51, MISO IO50, CSn IO49, GDO0 IO5), so the wireless socket stays free for the sensor radio; those five pins stop being wired zone terminals. Refused when the board doesn't offer it, when the result fails the board validator (with the clock module's stored place), or when a zone uses one of its pins; `rf pin` applies to the board's own place only. `rf` prints `place=` | `components/core/board_info/test` (BRD-014), `components/boards/board_crowpanel_p4/test` (`test_j7_places`), `tools/tests/test_flasher_parts.py` |
| RF-020 | **`rf module off`**: no receiver fitted - no pin configured, nothing probed (the `auto` search probes the socket every 30 s, which a Zigbee module fitted there must not see); the source stays registered, never usable, and `rf` says "433 MHz receiver: off" | `tools/tests/test_flasher_parts.py` (commands and replies); firmware review |
| RF-021 | **A zone's tamper and low-battery codes** (B012): the tamper and low-battery inputs of a zone whose sensor is a 433 MHz one are codes of that sensor, and the installer picks which: the page lists the codes the sensor sent while it was open, one row each, in the order heard, with how often and how long ago ("Code A · Sent third, twice · 6 s ago"), the zone's own code greyed; the code that ended the zone since start-up is its closed code, marked "Closes the zone" and never pickable (a contact may send its closed code when the cover comes off too). It says to open and close the door once, then take the cover off, and pick the code that appeared; when only door codes have come, it says the sensor can't report a tamper apart from the door and to leave the input as None. **Pick the code by hand** offers the protocol's codes less those the zone uses (a low-battery code can't be made to happen on the spot). Sensor settings show the code each input took; `zone <n> source` prints them. A low-battery code picked at the panel holds 12 h (`ZSRC_RF_BATTERY_HOLD_S`) | `test_rf.c` (`test_a_zones_other_inputs_list_its_sensors_codes`, `test_a_cover_that_sends_the_closed_code_offers_nothing`, `test_the_picker_says_how_long_ago`, `test_the_codes_query_is_only_for_rf`); preview `130_rf_tamper_codes`, `131_rf_battery_by_hand` |
| RF-022 | **A binding's hold time** (`hold_s`): the zone page's Sensor settings have a **Hold** row for a zone whose sensor is a 433 MHz one (and a Z-Wave momentary input), with the steps Automatic, 30 seconds, 1 minute, 5 minutes, 1 hour and 12 hours (Automatic is 0: the source's own 30 s, ended by the door's closed code; any other step only ever times out). The Panel Manager's binding form has the same field in seconds (Timing), shown for those two sources and not for a wired input, and the panel keeps it when the form is saved (the form used to drop it). A low-battery code newly bound to a 433 MHz sensor in the form holds 12 h unless another value is set, as at the panel (RF-021); a tamper code keeps Automatic, its trouble memory staying until acknowledged | preview `135_rf_hold`; `panel_pages.test.cjs` ("the binding form has a hold for 433 MHz and Z-Wave, keeps it when saved, and defaults a new low-battery code to 12 h") |
| RF-023 | **The newest code wins** (B020): a frame continues a transmission only while its transmitter has sent no other confirmed frame since. A door opened, shut and opened again inside `ZSRC_RF_REPEAT_MS` sends open, closed, open: the second open is a new transmission and the zone ends open; a bouncing magnet leaves the zone where the sensor's last code says. A lone other frame (a copy with a bit flipped, never confirmed) still splits nothing (RF-009). Ordered by arrival, not time | `test_rf.c` (`test_open_shut_open_at_once_ends_open`, `test_a_bounce_ends_on_the_last_code`, `test_a_lone_other_code_does_not_split_a_transmission`) |
| RF-026 | Installer > 433 MHz lists the receiver's state (Working, Interference, Not connected, Off) and when it last heard a sensor, whether 433 MHz sirens can be sounded, every zone whose sensor is on 433 MHz with its code and when it was heard, and the 433 MHz sirens (docs/features/16 OUT-010..OUT-014); words change at most once a second | preview (`bl-037-rf`); the mockup's `screens/sirens.js` (approved 2026-10-20) |
| RF-027 | The panel transmits too (docs/features/16 OUT-022): the CC1101 goes to TX between receptions (PATABLE +10 dBm, the line on GDO0 played by the RMT, `rf_tx.c`) and is tuned back to RX after; a receiver module can't, so a transmitter module's DATA on a terminal does (`rf txpin <gpio>`, NVS `rf`/`txpin`, taken from the zones' terminals). `rf send <addr> <code> [frames]` is the bench's test remote (BL-094) | HIL (BL-037) |
| RF-024 | **A slow transmitter fits one reception** (B025): every protocol at the slowest T its decoder takes fits in one reception - the receiver's idle (`RF_RECEPTION_IDLE_US`, 30 ms, the capture's `RF_CAPTURE_IDLE_US`) is longer than its longest space (EV1527's 31 T sync at 800 us: 24.8 ms; HT12E's 36 T pilot: 28.8 ms) - so a sensor that is slow, or slows as its 12 V cell sags, is heard; the two constants are asserted equal, and the capture's within the RMT's 15-bit idle counter | `test_rf_decode.c` (`test_every_protocol_at_its_slowest_fits_a_reception`); `test_rf_devices.c` (`test_a_sagging_cell_is_still_heard`, the `ev1527_t740` profile) |
| RF-025 | **The receiver on your own pins** (2026-10-04, ADR-048): `rf place custom sck=<g> mosi=<g> miso=<g> cs=<g> gdo0=<g>` for a CC1101 or `rf place custom data=<g>` for a receiver module (OOK), keys in any order, each once, decimal GPIOs, used from the next start (NVS `rf`/`place` = `custom` and `u8` keys `c_sck` `c_mosi` `c_miso` `c_cs` `c_gdo0` or `c_data`, the place erased first and written last). Accepted only on pins a person can reach (J7's IO pins on the CrowPanel), none twice, none the board's or held by another part's stored place (named or custom, `rf pin`, the Z-Wave reset line); a zone's wired terminal may be taken (that zone shows Check, ZSRC-010). Replies start "place custom from the next restart (`restart`)" or "not custom: "; a store that fails part-way puts the place, mode and pins there were before back ("the receiver stays as it was"). The place decides the module: `rf`/`mode` stays `off` (older firmware opens nothing), `rf module cc1101|ook|auto` is refused ("not <m>: the receiver is on your own pins ..."), `rf module off`, `rf place default` or a named place take custom away. Read back as "custom sck=.. mosi=.. miso=.. cs=.. gdo0=.." or "custom data=.." in `rf`, `rf status` and `rf place`, whose listing has a "custom: ..." line on a board with reachable pins; the status line says `place=custom`. Failing closed: a stored layout the board refuses at start (a pin out of reach, missing or damaged keys, a clash) leaves the receiver off, never on its own pins, and `rf` says "custom place not used: <why>" | `components/core/board_info/test` (BRD-021, BRD-022), `components/core/zone_hub/test` (`test_terminal_no_longer_offered_is_check`); console replies by firmware review; bench: not yet |

## Learning a sensor (2026-09-24, RF-009..011)

**The problem:** EV1527 has no checksum, so a frame with a bit flipped - common with a
superheterodyne module next to a switching converter - shows up as another sensor at the same
moment, a corrupted frame in the middle of a burst splits the real transmission, and a
neighbour's remotes and weather stations join the list. A picker that lists every first frame,
one row per (sensor, code), makes the installer play around until the right sensor is found.

**What others do:** a panel that learns a wireless sensor takes it from its repeated
transmissions and shows the one sensor it heard, with a confirmation;
rc-switch accepts a code only on its second reception ([RCSwitch.cpp](https://github.com/sui77/rc-switch/blob/master/RCSwitch.cpp));
rtl_433's EV1527 flex decoders ask for `repeats>=3` ([conf](https://github.com/merbanan/rtl_433/blob/master/conf/EV1527-4Button-Universal-Remote.conf)).
EV1527 door contacts usually send one ID with code A when opened and E when closed
([Home Assistant community](https://community.home-assistant.io/t/open-close-door-sensor-ev1527/37639)).

**What the panel does:**
- Only a repeated frame is a transmission (RF-009); several are followed at once.
- One row per sensor, its codes listed; sensors heard shortly before learning are set apart
  (RF-010).
- The picker says what it heard ("Listening: nothing heard yet", "Heard Sensor 89288. Close
  the door (or press the button again) to be sure.") and, once one sensor answered twice more
  often than any other, "Found it" with **Use Sensor 89288** and a beep (RF-011).
- Try it on the PC: `pwsh tools/ui_sim.ps1`, buttons "Neighbour's 433 MHz remote" and
  "Corrupted 433 MHz frame" next to "433 MHz contact opens".

## Protocols (RF-012, 2026-09-24)

All OOK at 433.92 MHz; timings in units T of the transmitter's own oscillator (T is measured on
every frame, so a sagging battery - EV1527 about +30 % at 5 V - does not matter).

| `rf_proto_t` | Chips / devices | Frame | Device, data | Check | Sources |
|---|---|---|---|---|---|
| 1 `EV1527` | EV1527, HS1527, RT1527, PT2262 / SC5262 tri-state; Sonoff DW1, most cheap contacts and PIRs | sync 1T+31T, '0' 1T+3T, '1' 3T+1T, 24 bits | 20-bit address, 4 data bits (one contact E open / 7 close / B tamper / F battery; another A open / E close / 6 battery - which is why codes are learned, not assumed) | none | EV1527 and PT2262 datasheets; rc-switch protocol 1; rtl_433 `generic_remote.c` |
| 2 `HT6P20B` | Holtek HT6P20B/D (South American alarm sensors and remotes) | inverted: pilot 23T low + 1T high, '0' 1T low + 2T high, '1' 2T low + 1T high, 28 bits | 22-bit address, 2 data bits; anti-code 0101 checked | framing only | Holtek datasheet; rc-switch protocol 6 |
| 3 `HT12E` | Holtek HT12E, SM5212 (older remotes) | inverted: pilot 36T low + 1T high, bits as HT6P20B, 12 bits | 8-bit address, 4 data bits | none | rc-switch protocols 11, 12 |
| 4 `KeeLoq` | Microchip HCS200/201/300/301 (alarm key fobs) | preamble, 10 TE header, '1' 1TE+2TE, '0' 2TE+1TE, 66 bits LSB first, 39 TE guard | 28-bit serial, 4 buttons; 32-bit hopping code (replay check), VLOW, RPT | structure; replay refused | HCS301 datasheet DS21143; rtl_433 `hcs200.c` |
| 5 `arctech` | arctech remote switches, plug sockets and door contacts | sync 1T+10T, each bit a Manchester pair of 1T+1T / 1T+5T, end 1T+40T, 32 bits (36 in one door contact's open report: 4 dim bits after the 32, read and set aside) | 26-bit house + 4-bit unit, on / off | Manchester pairs | rtl_433's arctech decoders |

Not decoded yet: 345 MHz security sensors (Manchester + CRC-16, heartbeats), legacy 433 MHz
security contacts (CRC-8 poly 0xF5, heartbeat every 60-64 min), 319.5 MHz security sensors.
Their CRCs would make them `checked` (one frame acts) and their heartbeats would supervise them
(RF-015); rtl_433 has decoders with the formats. KeeLoq's 100 us TE modes are at the edge of
the 60 us de-glitch filter.

## What goes wrong on the air (research 2026-09-24) and the test for each

Sources: TI CC1101 datasheet SWRS061, DN022, DN505, errata SWRZ020; the EV1527, PT2262/PT2272,
HT6P20B and HCS301 datasheets; rtl_433 (decoders, issues 370, 785, 1392, 2939, 3611), rc-switch
(issues 50, 177), Tasmota / Portisch RF Bridge, ESPHome remote_receiver, rtl_433_ESP (issues 16,
210, 228), OpenMQTTGateway; Logan Lamb, "Home Insecurity" (DEF CON 22); EN 50131-5-3
(free sample), NFPA 72 23.18; FCC 15.231. Where a figure below is ours (arithmetic, a model), it
says so.

| # | What happens | What it does to the pulses | What the panel does | Test |
|---|---|---|---|---|
| 1 | Slicer / AGC moves edges (strong signal: marks longer; weak: shorter) | each mark +-d us, its space -+d: the pair's length stays | unit from the whole sync pair; nearest template; up to ~0.6 T (EV1527), ~0.3 T (1T/2T protocols) | `test_edges_moved_by_the_slicer_still_decode`, `test_every_protocol_survives_the_slicer`, `..._too_far_lose_frames_not_codes` |
| 2 | Noisy slicer, CC1101's 8x-oversampled output (22-33 us steps) | random jitter per edge | 12 % of T decodes all; 30 % loses frames, never makes codes | `test_jittered_edges` |
| 3 | Superhet AGC turned up on the empty band: first frame(s) chopped (RXB6 AGC settles in ~0.17 s) | first pulses short / merged | the frame is lost; the repeats decode. A burst under ~0.17 s plus three frames can lose all but one (a Sonoff RM433's 0.19 s, a 6-frame HT6P20B or EV1527 PIR in the mocks): a superhet's limit, not a CC1101's | `test_the_first_frame_lost_to_the_agc`; every mock device (`test_every_device_survives_the_receiver`, which prints the short ones) |
| 4 | AGC static on an empty band (~2000 edges/s) | random pulses 40 us - 15 ms | never the same frame twice (a transmission) in ~17 min of it | `test_static_never_makes_a_transmission`, `test_garbage_...`, synthetic capture `synthetic_static_only.txt` |
| 5 | Noise spikes cut into pulses; CC1101 37 ns output glitches | a short opposite pulse inside a mark or a gap | healed below 60 us; a longer one loses its frame | `test_a_glitch_inside_a_pulse_is_healed`, `test_spikes_in_the_sync_low_are_healed`, `test_glitches_anywhere_are_healed`, `test_long_glitches_lose_frames_not_codes` |
| 6 | RMT overrun: the task fell behind, symbols dropped mid-frame | a pair missing: the old decoder shifted it into another code | exact frame length between syncs; a mark that may be the next sync's is never the last bit | `test_a_lost_pulse_pair_never_shifts_into_another_code` (every position, both frame orders), `test_lost_pairs_never_make_another_code_in_any_protocol`, `test_an_extra_pair_breaks_its_frame` |
| 7 | A bit flipped (EV1527 has no checksum; field reports of 2-bit data-nibble errors, Tasmota #3527) | a valid frame of another code | zones act on two identical frames (RF-014) | `test_a_flipped_bit_is_another_code_once`, `test_a_lone_frame_moves_no_zone` |
| 8 | Two sensors at once (door + PIR); OOK has no capture effect: carriers OR | mixed pulses | per-frame consistency refuses mixtures; the frames alone on the air decode | `test_colliding_transmitters_never_mix_into_a_third_code`, `test_a_collision_where_one_finishes_first`, `test_two_sensors_interleaved_both_count` |
| 9 | Two transmitters with the same clock, aligned (the limit of a code without checksum) | OR of the two codes, frame after frame | an unknown address (ignored unless learning); learning names the sensor that answered most | `test_two_aligned_transmitters_can_make_a_third_code` |
| 10 | Other devices on the band (weather stations, Manchester, other remotes) | foreign timings | no protocol reads another's frames | `test_protocols_do_not_read_each_other`, `test_foreign_signals_never_decode_twice` |
| 11 | Battery sagging / cold: RC oscillator slower (EV1527 +7 % at 9 V, +31 % at 5 V of 12 V) | T drifts within a burst | T per frame | `test_a_drifting_oscillator_decodes`, `test_every_transmitter_speed_decodes` |
| 12 | PT2262 sends its sync after the code word; bursts end on a sync mark | trailing sync | exact length; first word (no leading sync) lost | `test_a_sync_after_the_code_word_pt2262`, `test_the_last_frame_of_a_burst_ends_on_the_silence` |
| 13 | Upside-down line (a module idling high) | inverted levels | nothing decodes (`CONFIG_ALARM_RF_INVERT`) | `test_an_inverted_line_decodes_nothing` |
| 14 | A carrier on the band (jammer, stuck transmitter, LPD433 walkie-talkie) | the line held high / RSSI up | jamming counted 30 s in a minute however chopped; frames around it decode | `test_a_carrier_decodes_nothing`, `test_a_pulsed_jammer_is_still_jamming`, `test_a_busy_band_is_not_jamming` |
| 15 | Replay of a recorded fixed code (CVE-2019-9659, CVE-2023-31759) | a genuine frame | fixed codes: not trusted for fire/CO/panic (RF-006); rolling codes: a repeated hop refused (RF-016) | `test_a_replayed_rolling_code_is_refused` |
| 16 | A stuck or dying transmitter (an EV1527 contact at ~1 V sends every 3 s) | one endless transmission | one burst counted; zone active while it lasts, restores after its hold | `test_a_stuck_transmitter`, `test_a_burst_far_beyond_a_finger_is_noted` |
| 17 | Door opened and shut within a second (field reports of the open being lost) | open then close bursts back to back | both count, in order | `test_a_door_opened_and_shut_at_once` |
| 18 | Duplicate / factory addresses (PT2262 with open address pins; EN 50131-5-3 asks 10^6 codes at grade 2 - EV1527's 2^20 just meets it) | another sensor's code | marked "common factory address" | `test_common_factory_addresses` |
| 19 | No heartbeat from cheap sensors (supervision needs one) | - | any transmission of a bound sensor is a check-in | `test_every_transmission_is_a_check_in` |
| 20 | CC1101: wiring fault, clone, brown-out (GDO0 back to its 135 kHz clock), deaf after hours, torn RSSI reads | no pulses / a clock storm | read-back verify, reset check every 5 s, hourly recalibration, RSSI read twice | `test_cc1101_regs.c`; the storm pause (below) |
| 21 | A slow transmitter: a sync space longer than the receiver's idle (B025; a Tasmota capture's sync is 22.9 ms, a 12 V EV1527 at 5 V is T ~655 us) | the sync becomes the end of a reception | the idle (30 ms) outlasts every protocol's longest space at its slowest T (RF-024) | `test_every_protocol_at_its_slowest_fits_a_reception`, `test_a_sagging_cell_is_still_heard` |
| 22 | A short burst wholly inside a longer one (a PIR's 1 s inside a contact's 2 s) | every frame of the short one mixed | the longer one counts, nothing else is made of it; the short one is lost (its next trigger or supervision remains) | `test_a_burst_inside_a_longer_one_is_lost`, `test_two_sensors_overlapping_both_count` |

Real captures: `components/core/rf_decode/test/rf/` decodes every `*.txt` in it (the Flasher's
"Record 3 seconds" format with an `# expect:` line); the two there now are marked synthetic.

## Mock devices (2026-10-04)

The decoder's tests prove it on frames its own encoders make; `test_rf.c` proves the source on
packets handed to it. The **mock devices** (`components/core/zone_hub/test/rf_mock.c`) are the
products people buy, each as what it puts on the air: its chip's frame with that product's own
timings, the code it sends for each event, how many frames, and its habits. They play as pulses
through a model of the air and the receiver into the real decoder and the real source, driven
through the real hub (`test_rf_devices.c`), as the rf task does on a panel.

| Kind | Mock devices | Habits modelled |
|---|---|---|
| Contacts | EV1527 contacts that send 6 / 9 (`ev1527_contact_69`, the bench's), E / 7, A / E, A open only and 7 / C, a 12 V EV1527 at T 500, a slow EV1527 (T 740, a Tasmota capture), an arctech door contact, a Holtek HT6P20B contact | open and closed codes (or open only), tamper and low-battery codes, a cover that re-sends the door's code (B012), a cell that sags and slows the oscillator |
| Motion | two EV1527 PIRs (T 330 and T 400), Sonoff PIR3-RF, a driveway sensor (T 188) | asleep after a trigger (5 s; the Sonoff needs a quiet minute) |
| Smoke and water | an EV1527 smoke alarm, a smoke alarm with a wake-up pulse (its own PWM and stop pulses), two EV1527 water sensors | the code sent again every 10 s while the alarm lasts |
| Remotes | a 4-button EV1527 remote, Sonoff RM433, an EV1527 alarm remote (arm, disarm, home, alarm), an HCS301 KeeLoq fob | buttons as codes of one device; the fob's hopping code, its VLOW |
| Neighbours (must never decode) | Nexus-style and LaCrosse TX141TH weather sensors, two contacts of other alarm systems (433 MHz, PPM 41 bits and PWM 37 bits) | heartbeats every ~50-60 s, their own framing |

Each profile says where its numbers come from (rtl_433's decoders and flex configs, sellers,
Tasmota captures, our bench) and what is assumed; only the bench's 6 / 9 contact has been on our
air. The air: overlapping transmissions add up (OOK has no capture effect); the receiver can move
the edges (a strong or weak signal, +-80 us), jitter them, lose the start of a burst to a
superhet's AGC (0.17 s), hiss on an empty band (~2000 edges/s), and ends a reception after its
idle. The tests: every device decodes as itself, through every receiver; the neighbours never
decode, alone or together, on a clean or hissing band (10 minutes each); each sensor is found
while learning, a door among neighbours too; contacts open and close their zones; the 6 / 9 contact's cover
changes nothing; tamper and battery codes reach their inputs; a PIR asleep sends nothing; smoke
and water hold their zone while they repeat; a remote's buttons are one device's codes and a panic
button sets off its zone; a KeeLoq replay is refused; a sagging cell is still heard (B025); two
sensors overlapping both count, and a burst inside a longer one is lost.

Limits they show, kept in writing: a short burst through a slow-AGC superhet (row 3); a burst
wholly inside another's (row 22); the last frame of a burst whose last bit is 0 (its mark is as
short as a sync's, row 12) and the first frame of an HT6P20B or HT12E burst (its pilot is the
silence) are not decoded, so a burst yields one frame fewer than it sends. A new product: add a
profile with its sources; the tests take every profile.

## Design (as built, 2026-09-22)

```
CC1101 (SPI2, OOK, async serial)  --GDO0-->  RMT RX (1 us, glitch filter, partial receive)
                                                  |  ring buffer (internal RAM), task notification
                                             rf task (rf_service/src/rf_service.c)
                                                  |  rf_decoder_pulse() / rf_decoder_gap()
                                             rf_decode (portable): every protocol's frames -> packets
                                                  |  zsrc_rf_packet(), zsrc_rf_noise()  [zone_service lock]
                                             zsrc_rf (zone_hub, portable): learn table, watches, holds, jamming
                                                  |  sink.input() / sink.link()
                                             zone_hub -> the engine (zone events), the picker, the zone page
```

- `components/drivers/rf_rx` (ESP-IDF): `cc1101.c` - manual CS with the CHIP_RDYn wait, the
  power-on reset, identification (PARTNUM 0, a VERSION that is stable and neither 0x00 nor
  0xFF), the OOK register table for a band, calibration and RX, RSSI in dBm; `rf_capture.c` -
  an RMT receive channel with the 3 us hardware glitch filter, 30 ms idle ending a reception (B025), partial
  receive into a 1024-symbol ring, re-armed from the interrupt. The classic ESP32 (the 4" board)
  has no ping-pong receive, so there a reception is handed over whole (4 memory blocks, about ten
  EV1527 frames; a longer burst continues as the next reception), and its input-only data pin
  (IO35) gets no internal pull-down (B009). A capture that won't start is retried with the next
  30 s probe, not every 100 ms poll.
- `components/core/rf_decode` (portable C11; rewritten 2026-09-24, RF-012/013): a streaming
  decoder fed one pulse at a time. A de-glitch filter first (pulses under 60 us merge into
  their neighbours; it holds one pulse back). Then every protocol of "Protocols" runs its own
  copy of one pair engine: a frame is a sync pair (T = the pair's whole length / its length in
  T) then exactly N (mark, space) pairs; each pair is the template it is nearest to by a
  quarter of their distance, within 30 % of its length; the frame ends at the next sync, a
  gap, or anything that is no bit, and is handed out only with exactly N bits whose marks,
  spaces and lengths of each kind agree within max(0.8 T, 160 us) (lengths: max(0.6 T, 150 us))
  and that pass the protocol's own rules. A lone mark before silence is the last bit only
  when it cannot be the next sync's mark. `rejected` counts frames that broke after 8 bits or
  more. Encoders for every protocol (`rf_encode`, `rf_encode_burst`, `rf_packet_make`) make
  the tests' frames (the PC simulator does not use them: it hands packets straight to
  `zsrc_rf`, see "PC" below). About 1.8 KB of state per receiver.
- `components/core/cc1101_regs` (portable, RF-017): the receive table per band, the frequency
  word, RSSI, identification, read-back verification, reset detection; `rf_rx/cc1101.c` does
  the SPI with it.
- `zsrc_rf` (`components/core/zone_hub`): the address of a binding is one **(device, code)**:
  `kind RF, id = protocol << 32 | device (the protocol's width), input = code + 1`. Since
  RF-014 a code acts only when its transmission is confirmed (its frame twice; once for a
  protocol with a checksum); every confirmed transmission of a bound device is a check-in
  (RF-015); a KeeLoq hop heard before is refused and its battery bit reaches the zone (RF-016).
  The zone is active
  from the moment its code is heard and quiet again when the device sends another code no
  binding of the zone claims (its closed code), or after the binding's `hold_s` (a new
  mapping field, blob item `0x0C`; 0 = automatic: 30 s). An explicit hold ignores other codes
  (a PIR's low-battery chirp must not end its motion). Tamper and low-battery codes are the
  zone's tamper and battery inputs with their own holds. A transmitter's repeats inside
  500 ms are one transmission; a transmission over 100 frames is counted as a burst (RF-006).
  Every confirmed transmission, learning or not, goes into a 16-entry table of sensors
  (`ZSRC_RF_CAND_MAX`), each with the codes it sent, so the sensors already transmitting
  before learning began can be set apart. The picker shows one row per sensor: those heard
  while learning first, newest first, then the ones heard shortly before (RF-009..011). A
  receiver that stops answering or a carrier of -55 dBm or louder for 30 s takes the source's
  link down (RF-005: a sample within 500 ms of a sensor's frame is that sensor talking, unless
  its transmission has gone past 100 frames); the hub reports that as the source's trouble
  after its 60 s grace (ZSRC-015), and the source re-reports what it knows when the link
  returns, since one-way sensors cannot be asked.
- `components/services/rf_service/src/rf_service.c`: the task (internal 4 KiB stack, priority 4) probes
  the bus every 30 s until a CC1101 answers, tunes it, starts the capture, drains pulses into
  the decoder, hands codes to the source under the zone service's lock, samples RSSI every
  200 ms between frames - and at least once a second when the line never goes quiet, since a
  noise jammer keeps the demodulator toggling - and checks every 5 s that the chip is still in
  RX. Console `rf`:
  `status`, `retry`, `freq <kHz>` (remembered in NVS `rf`/`khz`), `raw on|off` (log every
  transmission), `capture <ms>` (dump the raw pulses: the way to record fixtures).
- The receiver module (see below): no SPI; the capture runs on the data pin with a pull-down,
  the task counts edges and the line's duty every second (a level that holds makes no pulses,
  so the time since the last pulse counts at the level the pin reads now - that is how a
  carrier holding the line high shows; such a line is not "silent"), and a flood of more than 20,000
  edges a second (a clock on the pin, e.g. a CC1101's GDO0 before it is configured) pauses the
  capture for 10 s so its interrupt cannot swamp the panel. The RMT glitch filter is 3 us (its
  hardware limit is 255 ticks of the 80 MHz group clock); the decoder drops and stitches the
  rest.
- Kconfig: `CONFIG_ALARM_ZSRC_RF` (default on; harmless without the module), `ALARM_RF_MODULE`
  (auto / CC1101 / module; `rf module` overrides it from the next restart), the pins
  `CONFIG_ALARM_RF_SPI_MOSI/MISO/SCK/CS_GPIO`, `CONFIG_ALARM_RF_GDO0_GPIO`,
  `CONFIG_ALARM_RF_FREQ_KHZ`, `CONFIG_ALARM_RF_INVERT`.
- The picker: "433 MHz sensor" appears only while a receiver answers (ZSRC-005); its page
  says "Trigger the sensor: every code it sends appears here" and explains the closed code and
  the 30 s. The zone page's Source row reads "433 MHz · A2B3C code 5"; its trust facts say not supervised, replayable, not for fire/CO/panic.
- PC: the simulator's buttons "433 MHz contact opens / Neighbour's 433 MHz remote / Corrupted
  433 MHz frame / tamper code / battery code / CC1101 unplugged / band jammed" drive the real
  source; the preview walks the learn flow. The buttons that send a code (contact, neighbour,
  corrupted frame, tamper, battery) hand the source packets as the decoder would
  (`zsrc_rf_ev1527()` into `zsrc_rf_packet()`, in `host/ui_preview/host_panel.c`): no pulses go
  through `rf_decode` there, whose own tests cover the air. "CC1101 unplugged" and "band
  jammed" call `zsrc_rf_receiver()` and `zsrc_rf_noise()`.

### Wiring (CrowPanel Advanced 7", board V1.1)

| CC1101 pin | Socket pad as printed (board net) | P4 GPIO | Kconfig |
|---|---|---|---|
| VCC | 3V3 | - | |
| GND | GND | - | |
| SI (MOSI) | IO6 (SPI2 MOSI) | 6 | `ALARM_RF_SPI_MOSI_GPIO` |
| SO (MISO) | IO7 (SPI2 MISO) | 7 | `ALARM_RF_SPI_MISO_GPIO` |
| SCLK | IO8 (SPI2 SCK) | 8 | `ALARM_RF_SPI_SCK_GPIO` |
| CSn | IO9 (IO9_W) | 9 | `ALARM_RF_SPI_CS_GPIO` |
| GDO0 | IO10 (IO10_W_BUSY) | 10 | `ALARM_RF_GDO0_GPIO` |
| GDO2 | not connected | | |

Pad names as printed beside the socket (the V1.0 board photo and the schematic's J9/J11 nets,
docs/research/AddOnWiringResearch.md; to be confirmed on a V1.1 board). The ESP32-H2
module's own pin names (IO0/IO1/IO2/IO5/IO4) are not these. **On the J7
header instead (RF-019):** `rf place j7` - SCK IO52, MOSI IO51, MISO IO50, CSn IO49, GDO0 IO5,
3V3 and GND, leaving the socket to the sensor radio; the Flasher draws it.

The socket is the ESP32-H2's place later (docs/features/20); the H2 only uses the UART pads, so
the two can share the pads electrically, but not the physical socket: when the H2 arrives the
CC1101 moves to wires on the same nets, or to other pins (RF-019's `rf place j7`; the SPI
goes through the GPIO matrix, any free pin works; keep GDO0 off strapping pins). A 433 MHz
antenna: 17.3 cm of wire, or the module's spring.

### The receiver module instead (D-FLIFE kit, Amazon B0BZRRBBNK)

The kit is five WL102-341 transmitters and five superheterodyne receivers (RX470-4, the
improved WL101-341: 433.92 MHz, ±150 kHz, -108 dBm, 2.2-5 V, ASK/OOK, digital output), with
antennas ([listing](https://www.amazon.com/D-FLIFE-Wireless-Transmitter-Receiver-Antenna/dp/B0BZRRBBNK),
[WL101-341 data](https://www.motorobit.com/wl101-341-433mhz-rf-receiver-module)). Only the
receiver is used; the transmitters are handy on the bench as test remotes (drive one with
`rf_ev1527_encode()` pulses, a later task).

| Receiver pin | Socket pad | P4 GPIO |
|---|---|---|
| VCC | **3V3** (never 5V: DATA swings to VCC and the P4's pins are not 5 V tolerant) | - |
| GND | GND | - |
| DATA (either DATA pin if it has two) | IO10 (IO10_W_BUSY) | 10 (`ALARM_RF_GDO0_GPIO`) |
| ANT | the long (receiver) antenna, or 17.3 cm of wire | - |

**On the J7 header instead (RF-008; the second panel, DATA on IO3 = GPIO 3):** any wired terminal can take the data line: `rf pin 3` on the console (or click
IO3 on the RoboAlarms Flasher's 433 MHz page, docs/features/34), then `restart`. That terminal is
then no longer offered for wired zones; `rf pin` alone says where it listens, `rf pin default`
puts it back on the socket's pad IO10 (GPIO 10). Power the module from **3V3** there too. `rf find` (watch every
free pin for the module's static and name it) is not built: the pin is known.

No SPI, no RSSI. The service finds the module by what its AGC does to an empty band - a
steady stream of static on DATA, where an unwired pin (pulled down) shows no edges at all -
and supervises it the same way (five minutes without a single edge: gone). A carrier on the
band holds DATA high, so the line high for more than 85 % of a second, for 30 s, is jamming.
All three numbers are from the module's behaviour in general, not measured on this one:
`rf status` shows `edges/s` and `high=%` for the bench. A module with a squelch (quiet DATA)
would never be found by `auto`: then `rf module ook` and a restart.

### First bench (the steps)
1. Wire as above, power the panel, `rf` on the console: expect "CC1101 found (version 0x14)"
   in the log and `Receiving at 433.920 MHz, rx, noise -9x dBm`. "no CC1101 answered": check
   3V3, CSn and SO; `rf retry` after fixing.
2. `rf raw on`, press a 433 MHz remote or trigger a sensor: expect
   `EV1527 XXXXX code N (XXXXXN, T 300-400 us)` lines, one per press, with `frames` growing
   by 4-20 per press in `rf status` and `rejected` staying small. Nothing: `rf capture 3000`
   while pressing and read the pulses (a healthy frame is H~T L~31T then pairs of
   H~T/L~3T and H~3T/L~T); all upside down → `CONFIG_ALARM_RF_INVERT`; nothing at all →
   GDO0 wiring or IOCFG0; only noise → the frequency (`rf freq 433920`, or 315000 for a
   315 MHz remote).
3. Zones > Add a zone > 433 MHz sensor. With the door closed, open it and close it: the
   picker says "Found it" and offers **Use Sensor XXXXX** (RF-011). Open and close it:
   the zone follows. Record the noise floor `rf status` shows to settle the jamming
   threshold (RF-005, currently -55 dBm).
   With the receiver module instead: `rf` should say `Receiver module on GPIO 10, NNNN
   edges/s, line NN% high` within a second of power; then steps 2-3 are the same (`rf freq`
   does nothing for a fixed module).
4. Save a few `rf capture` dumps of real sensors under `components/core/rf_decode/test/rf/` as fixtures for
   `test_rf_decode.c` (the decoder's timing limits are set from datasheets, not from air).
5. With the B011 fixes: while a sensor is held down, and while the Flasher's
   433 MHz page polls `rf`, the log shows no "receiving OOK at ..." lines between frames (each
   is a retune) and `rf` keeps `spi_errors=0 verify_errors=0 resets=0`. `state=` in `rf` is the
   state at the task's last check (at most 5 s old). The RMT interrupt waits while flash is
   written (`rf_rx` README): if a transmission is ever missed right after an event is logged,
   that is the place to look.
6. A zone's tamper and low-battery codes (RF-021): Zones > the zone > Sensor settings > Tamper
   input > 433 MHz sensor, door closed, take the cover off, pick the code that appeared;
   `zone <n> source` shows it. **A low-battery code** only comes when the battery is low. On
   the bench, feed the sensor less than its cell: a coin-cell (CR2032) contact from a bench
   supply at 3.0 V turned down in 0.1 V steps to about 2.2 V, or from two AA cells through one
   silicon diode (about 2.5 V), wired to the cell holder's + and - (never above 3.3 V); open
   and close the door at each step and watch `rf raw on` for a new code. Many cheap contacts
   have no low-battery code at all: if nothing new appears before the sensor stops sending, it
   has none. Pick the code by hand when the seller gives one.

**Bench results, 2026-09-25 evening (a panel; B011, B012):** CC1101 on J7 answers
(version 0x14, `state=rx`, no SPI or read-back errors, no retunes during transmissions); noise
floor -83 to -84 dBm; a door contact (CR2032, EV1527, T 420 us) reads -15 to -27 dBm
a metre from the panel and sends about 36 frames (two seconds) per event, some over 100; it sent
codes 6 and 9 only: 6 open, 9 closed, and 9 again when its cover comes off - it has no tamper
code of its own: its cover switch re-sends the door's state (6 with the door open, 9 with it
shut), when the cover comes off and when it goes back. Two slow rounds of door and cover with
`rf raw on` and back-to-back `rf capture 9000` dumps, read again by an independent decoder on the
PC, gave 220 whole frames, all that contact's address, code 6 or 9; the only other shapes were
pieces of those frames (a burst's first frame clipped while the AGC settles, or cut by the
window); `unknown=0 rejected=0 overruns=0`. Not yet: the fixtures (step 4), the low-battery code, the hourly
recalibration's log line (Tasks, BL-022). The B011 fixes (the rf task no longer retunes the
CC1101 mid-transmission, `rf status` no longer does SPI beside the rf task, FSTEST is written to
0x29 not PTEST) and B012's (a contact talking at the panel is not a jammer, RF-005; a zone's
tamper input no longer takes the door's closed code, RF-021) are in docs/bugs/B011 and B012. The
bypass regression (B014: RF source -> hub -> engine keeps updating through repeated bypass and
unbypass) passes; the reported physical-contact reception loss after a bypass is open (BL-022).

## Open questions
- What each sensor model actually sends, its quirks and ideas for handling them are kept in the
  [sensor notebook](../../sensors/README.md). Deferred there: an option for
  433 MHz zones to report a repeated door code as a tamper, for contacts whose cover re-sends
  the door's state instead of a tamper code of their own.
- Where the CC1101 lives once the H2 module takes the socket (wires to the same nets, or
  J7 pins with fewer wired terminals). Answered by RF-019: `rf place j7` puts the
  CC1101 on the J7 header, whose five pins then stop being wired terminals, so the socket
  stays free for the sensor radio.
- The jamming threshold (the bench, BL-022). The hold defaults per role are decided (RF-022):
  a low-battery code holds 12 h, at the panel and in the browser's binding form (where a new one
  gets it unless another value is set); a tamper code keeps the automatic hold, its trouble
  memory staying until acknowledged; the main sensor's hold is Automatic until the installer picks
  a step.
- 315 / 345 MHz: the CC1101 tunes there (`rf freq`), the decoders for the 345 MHz security
  sensors do not exist yet; a 433 MHz module's matching network at 345 MHz is untested (BL-039).
- **Acting on the second frame (RF-014) is a change the owner should know about:** a sensor
  at the edge of range whose burst gets only one frame through no longer moves its zone (the
  old firmware acted on the first). Cheap sensors send 4 to ~37 frames per event, so this
  should not be seen; if it is, the fix is the receiver's placement, not a single frame.
- The KeeLoq, HT6P20B, HT12E and arctech decoders are built from datasheets and rtl_433 /
  rc-switch, and proved against the encoders here: no real device of those families has been
  on the air yet (their captures: BL-039).
- The CC1101 table is unchanged (a 325 kHz filter): open-source receivers differ (Flipper
  270/650 kHz, rtl_433_ESP 812 kHz) and a sensor ~60 kHz off frequency was lost at 203 kHz in
  one A/B test. `rf capture` from real sensors should decide whether a wider filter is worth
  its sensitivity (the bench's captures, BL-022).

## Tasks
- [x] Pick receiver and pins (CC1101 on the socket SPI; Kconfig for anything else)
- [x] Receive-only driver diagnostics and a bounded timestamped capture export for the bench
      (`rf status`, `rf capture`); sanitized committed fixtures: after the first captures
- [x] `rf_decode` with EV1527 / PT2262 (+ the encoder for the tests; the PC simulator hands
      packets straight to the source)
- [x] Multi-protocol decoder (HT6P20B, HT12E, KeeLoq, arctech), de-glitch, strict framing,
      consistency; failure-mode tests; capture fixtures folder (RF-012, RF-013)
- [x] Zones on confirmed frames, check-ins, rolling-code replay, battery bit, jamming 30/60
      (RF-005, RF-014..016); CC1101 read-back, reset detection, hourly recalibration (RF-017)
- [ ] 345 MHz, legacy 433 MHz and 319.5 MHz security-sensor decoders (CRC: one frame acts;
      heartbeats supervise, RF-004) (BL-039)
- [ ] Real captures of each protocol family in `components/core/rf_decode/test/rf/` (the two
      there now are synthetic; the EV1527 contact's come with the bench below) (BL-039)
- [x] `rf_rx` CC1101 driver + RMT capture
- [x] Review before the first CC1101 bench (docs/bugs/B011): console requests apart from the
      capture's wake-ups, the chip the rf task's alone, FSTEST at 0x29 with a test
- [x] `zsrc_rf` with learn flow, holds, jamming; the picker; the simulator; the preview
- [x] Fixed receiver module (RX470-4 / WL101-341) on the data pin, found by its static
- [x] First bench, the CC1101's part: the register table receives on air, noise floor -83 to
      -84 dBm, B012 found and fixed (docs/bugs/B011 and B012, c88d544)
- [ ] First bench, what is left: sanitized `rf capture` fixtures of real sensors (step 4), the
      contact's low-battery code (step 6), the hourly recalibration's log line, the jamming
      threshold settled against the measured noise floor (RF-005, -55 dBm now), and the
      receiver module's edge rate and line duty (its presence and carrier thresholds) (BL-022)
- [ ] With a real contact on the panel: B012's fixes tried (a contact talking at the panel
      raises no "Sensor radio problem"; the tamper page marks and refuses the closed code) and
      B014's open and close still received after a bypass and unbypass (BL-022)
- [x] A sensor talking is not a jammer (RF-005, B012)
- [x] A zone's tamper and low-battery codes picked per code, or by hand (RF-021, B012)
- [x] `hold_s` in the zone page and the browser's binding form; per-role defaults (RF-022)
      (BL-056, 5153415)
- [x] Mock devices: 21 products and 4 neighbours through the air, the receiver, the decoder and
      the source (`rf_mock.c`, `test_rf_devices.c`); B025 found and fixed (RF-024) (f2dc2b2)
- [ ] B025 on a panel: the dev build's 30 ms idle with the bench's receivers, a contact still
      heard; a slow transmitter (T ~700 us) heard, from a bench supply or a WL102-341 (BL-163)
- [ ] Two receivers at once (RF-001) (BL-092)
- [ ] A keypad with a receiver as an RF zone expander for the master (RF-007, M7+; needs the
      panel link, BL-048) (BL-093)
- [ ] On a panel: `rf send` from the CC1101 and from a WL102-341 on `rf txpin` heard by a second
      receiver, the receiver back in RX after each (`rf status`), and a 433 MHz siren paired and
      sounding (RF-027, BL-037)
- [ ] A WL102-341 transmitter from the receiver kit as a bench test remote, driven with
      `rf_ev1527_encode()` pulses ("The receiver module instead") (BL-094)
