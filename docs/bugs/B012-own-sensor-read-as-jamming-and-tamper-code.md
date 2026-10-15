# B012 - The installer's own sensor counted as a jammer, and a tamper input took the closed code

**Status: fixed in code, host-tested and preview-checked. The first version (sections 1-2)
was flashed on a panel 2026-09-25 ~18:35 and tried there: the contact was found to
send its closed code when its cover comes off (section 3); the fix for that and the picker's
clock was flashed there at ~18:54, not yet tried.** Found
on the first CC1101 bench: a panel, a CC1101 on J7 (`rf place j7`), a DS3231 on the
I2C connector, and a one-way door contact (sold in threes, CR2032 cell; EV1527, T 420 us;
its address is not recorded here). The CC1101 itself is healthy: `version=0x14 state=rx(0x0D) spi_errors=0
verify_errors=0 resets=0`, noise floor -83 to -84 dBm between transmissions.

## 1. "Sensor radio problem" while testing a sensor at the panel

**Symptom (reported):** "I keep getting Sensor radio problem."

**Evidence (console on COM4, no reset, 2026-09-25 ~17:58-18:02):**
- The event log: the `sensor_radio` system trouble raised and restored again and again (for
  example raised 584 s after start and restored at 585 s, raised at 686 s and restored at
  710 s); `zones: source 5: its link has been down too long` in the log. Source 5 is the
  433 MHz source; nothing else was down but Home Assistant, which feeds no zone.
- `rf` a second apart: `noise -25 dBm`, then `noise -83 dBm`.
- With `rf raw on` and `rf` every 5 s for two minutes while the contact was opened and closed
  at the panel: every noise sample taken just after an `EV1527 <address> code 6` or
  `code 9` line read -15, -17, -26 or -27 dBm, every other one -83 or -84 dBm; 80 s in, `rf`
  said `CC1101: jammed` (`jammed=1`) with nothing on the air but that contact.
- The contact talks for about two seconds an event: 5,469 frames in 152 transmissions (about
  36 frames of 54 ms each), 6 of them over 100 frames.

**Root cause:** RF-005 counts a carrier "between frames". `rf_service.c` samples the RSSI every
200 ms once the line has been quiet for 100 ms, and at least once a second when it never is. A
CC1101's demodulated output toggles on the band's noise (960 edges a second on this panel), so
the line is never quiet and the once-a-second sample lands anywhere - often inside the
contact's two-second transmission, which a metre from the antenna is 30-40 dB above the
jamming threshold (-55 dBm). Each such sample counted a second of carrier; an installer testing
a contact keeps it on the air most of the minute, so 30 s of the last minute were soon reached,
the source's link went down and, after the hub's 60 s grace, the trouble came up. The host
tests fed carrier samples and frames separately, never both at once.

**Fix (`components/core/zone_hub`, portable):** a carrier sample taken within
`ZSRC_RF_TALK_MS` (500 ms) of a decoded frame is the sensor that sent it, not a carrier
(`zsrc_rf_carrier()`, `talk_at` set by `zsrc_rf_packet()`), unless that transmission has gone
past `ZSRC_RF_BURST_FRAMES` (100 frames, the RF-006 burst): a stuck transmitter blocks the band
like any jammer and still counts. The moment before a transmission's first frame is decoded
(about 100 ms) still counts, a few seconds a minute at most.

**Tests (`components/core/zone_hub/test/test_rf.c`, RF-005):**
`test_a_sensor_talking_is_not_jamming` (the bench's pattern: on the air 80 % of three minutes,
-20 dBm samples inside it; fails on the old code), `test_a_stuck_transmitter_is_jamming`,
`test_a_jammer_with_a_sensor_in_between_is_jamming`.

**Limit, in writing:** someone who fills the band with valid frames, changing code before any
transmission reaches 100 frames, is not counted as jamming (their frames are counted as
transmissions of unknown sensors). Replaying a fixed code is already possible for anyone with a
transmitter (RF-006); a jammer that sends noise or a plain carrier is still caught.

## 2. A 433 MHz zone's tamper input took the door's closed code

**Symptom (reported):** "when I setup the 433mhz tamper from the device it seems inverted."

**Evidence:** zone 10 "Test" is bound to the contact's code 6 (`zone 10 source`); the contact sent
only codes 6 and 9 in two minutes of testing. The hub refuses one code in two roles of a zone
("one sensor, one role"), so its tamper input is on another code - by elimination 9, the code
that is not the zone's own (the console printed only the main input then, so this is not read
back). Zone 10 also logged a tamper trouble at 433 s, restored at 572 s.

**Root cause:** a zone's tamper and low-battery inputs used the zone's sensor picker unchanged:
"Open the door, then close it", a list of sensors, and **Use Sensor XXXXX** binds the code the
sensor sent *first* while the page was open (RF-010). For a tamper that is whatever the contact
said first - its closed code, when the magnet moved or the door was shut. Bound as the tamper,
the closed code shows a tamper every time the door closes; and since a bound code restores
nothing but its own hold, the door's own zone stopped closing on it and waited out its 30 s
automatic hold instead. The zone's Sensor settings only said "433 MHz" for the input, so
nothing on the screen showed which code it had taken.

**Fix (RF-021):**
- The tamper and low-battery pages of a 433 MHz zone list the codes of the zone's own sensor,
  one row each, in the order heard ("Code 9 · Sent first"), the zone's own code greyed and not
  pickable (`zsrc_rf_codes_query()`; the RF source answers that query with one candidate per
  code). They say what to do ("With the door closed, take the sensor's cover off ... Pick the
  code that appeared as the cover came off"); when the first code is the zone's own, the page
  says the cover opens the zone and no tamper input is needed.
- **Pick the code by hand**: the protocol's codes less the ones the zone already uses, for a
  low-battery code that can't be made to happen on the spot.
- Sensor settings show the code each input took ("433 MHz · code 9"); the console's
  `zone <n> source` prints the tamper and battery inputs too.
- A low-battery code picked at the panel gets a 12 h hold (`ZSRC_RF_BATTERY_HOLD_S`): with the
  automatic hold, the battery trouble ended at the door's next closed code.

**Tests:** `test_a_zones_other_inputs_list_its_sensors_codes` (the zone's own code marked by its
zone, the closed code unusable, the tamper code picked and working beside the door),
`test_the_codes_query_is_only_for_rf`; preview `130_rf_tamper_codes`, `131_rf_battery_by_hand`
(appended: no earlier shot renumbered).

## 3. On the panel: this contact's cover sends its closed code, and "just now" never aged

**What was seen (2026-09-25 evening, the first version flashed):** on the new tamper page,
"Code 9 when I set off the Tamper, Code 6 when I open, Code 9 when I close. When I reset the
tamper the light on the Door Sensor lights up but it does not send anything", and the page said
"just now" for good instead of how long ago.

**What it means:** this contact (EV1527) has no tamper code of its own: taking its
cover off sends 9, the same code as the door closing, so no panel that reads codes can tell the
two apart. A tamper input on 9 shows a tamper every time the door shuts - the "inverted" tamper
of section 2 - and the first version of the page still offered 9 as "Sent first". Whether the
cover going back sends anything is not known: the page showed each code once, with no count, so
a second 9 would not have shown. Checked afterwards (Features/25 "Bench results"): the panel
receives everything the contact sends - 220 whole frames over two rounds, all the contact's 6 or 9,
nothing from any other address, nothing rejected - and its cover re-sends the door's state both
ways (6 with the door open, 9 with it shut). Its only protection: while the door is shut, taking the cover
off changes nothing; while it is open, the zone reads closed.

**The clock:** the picker's "12 s ago" was worked out from the time of the latest radio frame,
so the newest sensor read "just now" until the next frame (the committed `113_rf_learn` shows a
neighbour's remote, heard seconds earlier, as "just now" too).

**Fix:**
- The RF source notes which code ended each bound input (`closed_by`: its closed code since
  start-up); the codes page marks that code "Closes the zone" (`zsrc_candidate_t.unusable`, a new
  field any source may set) and it can't be picked, there or by hand. The page now says to open
  and close the door once first, so the panel knows the door's codes before the cover comes off.
  When every code heard is the door's, it says: "Only its door codes so far. If taking the cover
  off sends one of them, this sensor can't report a tamper apart from the door: leave Tamper
  input as None."
- Each code row shows how often it came ("Sent third, twice") and when ("6 s ago").
- The source keeps the hub's tick as its clock (`clock`, `latest()`), and while a picker is
  open it asks for a tick every second.

**Tests:** `test_a_cover_that_sends_the_closed_code_offers_nothing` (6 open, 9 closed, 9 again
for the cover: two rows, 9 unusable, heard twice), `test_the_picker_says_how_long_ago` (fails
with the old clock), the updated `test_a_zones_other_inputs_list_its_sensors_codes`; preview
`113_rf_learn` ("3 s ago" for the neighbour's remote), `130_rf_tamper_codes`,
`131_rf_battery_by_hand` refreshed.

## Verify on the panel
1. Flash (when asked to), then `rf raw on` and test the contact at the panel for two
   minutes: `rf` keeps `jammed=0` and no `sensor_radio` trouble comes up. Acknowledge the old
   restored trouble first (code + OFF).
2. Zones > Test > Sensor settings > Tamper input > 433 MHz sensor; open and close the door,
   then take the cover off: with this contact, 9 shows as "Closes the zone" and the page says
   to leave the tamper input as None. A contact with its own tamper code: pick it; `zone <n>
   source` shows it; opening and closing the door raise no tamper.
3. Low battery: see docs/features/25 "First bench" (a coin-cell contact on a bench supply or
   two AA cells through a diode), or pick the code by hand if its seller gives one.
