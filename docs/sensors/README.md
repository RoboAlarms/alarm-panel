# Sensor notebook

What real sensors do on the air, as seen on a panel: which messages they send, their quirks,
what RoboAlarms does with them, and ideas left for later. One
entry per sensor model, newest behaviour notes in the entry; the patterns that repeat across
models are gathered at the top so a new sensor can be recognised quickly.

This is a field notebook, not a specification: requirements live in the feature documents
(433 MHz: [docs/features/25](../features/25-RF-Sensors-315-345-433/README.md); Zigbee:
[docs/features/20](../features/20-ESP32-H2-Sensor-Radio/README.md), whose device quirks are also
in `components/core/zone_hub/src/zb_quirks.c`; Z-Wave:
[docs/features/36](../features/36-Z-Wave-Sensors-ZAC93/README.md)), and faults in
[docs/bugs](../bugs/README.md). When an idea here is taken up, it moves into its feature's
requirements and the entry points there.

## Patterns seen so far (433 MHz, EV1527 and alike)

A 433 MHz one-way sensor sends a fixed address and a few data bits; what the data bits mean is
the maker's choice, which is why the panel learns codes by watching them rather than assuming
(docs/features/25 RF-003).

| Pattern | What it sends | Seen on | What the panel does |
|---|---|---|---|
| Open and closed codes | one code when the magnet leaves, another when it returns | [an EV1527 door contact](#ev1527-door-contact-6-open-9-closed) (6 / 9); another (E / 7, rtl_433's decoder for it); another (A / E) | the zone's code opens it, any code no binding claims closes it |
| Open only | one code on opening, nothing on closing | many PIRs and cheap contacts | the zone closes after its hold (30 s automatic) |
| A tamper code of its own | a separate code when the cover comes off | a contact that sends B (rtl_433's decoder for it) | bind it as the zone's tamper input (RF-021) |
| **Cover re-sends the door's state** | no tamper code: the cover switch makes it send the door's current code again, when the cover comes off and when it goes back | [the 6 / 9 door contact](#ev1527-door-contact-6-open-9-closed) | no tamper input possible today; the tamper page marks the closed code "Closes the zone" (RF-021). Idea: [a repeated door code as a tamper](#idea-a-repeated-door-code-is-a-tamper) |
| A low-battery code | a separate code once the cell sags | a contact that sends F (rtl_433); another that sends 6 | bind it as the battery input, picked by hand or heard on a bench supply (RF-021) |
| No heartbeat | nothing between events | every EV1527 sensor so far | not supervised; any transmission is a check-in (RF-015) |

## Entries

### EV1527 door contact (6 open, 9 closed)

- **Sold as:** a wireless door/window sensor, a one-way 433 MHz contact sold in threes.
- **Power:** CR2032 coin cell (3 V). Its listing gives "Low voltage alarm: 2.2-2.4V".
- **On the air:** EV1527, T about 420 us (short pulse ~420 us, long ~1150 us, 13 ms between
  frames), about 36 frames (two seconds) per event, some bursts past 50 frames. Reads -15 to
  -27 dBm a metre from the panel's CC1101.
- **Messages:** code **6** when the magnet leaves (open), code **9** when it returns (closed).
  Taking the cover off (the tamper button) lights its LED for about 3 s and sends the **door's
  current code again**: 6 with the door open, 9 with it shut; putting the cover back does the
  same. It has no tamper code of its own. Low-battery code: not yet heard. Heartbeat: none seen
  (to confirm over a night).
- **Proven:** 2026-09-25 on a panel (CC1101 on J7): two slow rounds of door and cover
  with `rf raw on` and raw `rf capture` dumps decoded again on a PC - 220 whole frames, all 6 or
  9, nothing from another address, nothing rejected (docs/features/25 "Bench results",
  [B012](../bugs/B012-own-sensor-read-as-jamming-and-tamper-code.md)).
- **What RoboAlarms does (2026-09-25):** 6 as the zone's code; 9 closes it. Tamper input: leave
  it as None - a tamper on 9 would show every time the door shuts (B012). The tamper page lists
  9 as "Closes the zone" and says so. With the door shut, taking the cover off changes nothing;
  with it open, the zone reads closed afterwards (the sensor's limit).
- **Open:** its low-battery code (bench supply, docs/features/25 "First bench" step 6); whether it
  ever sends a heartbeat.

## Ideas, not planned

### Idea: a repeated door code is a tamper

For sensors whose cover re-sends the door's state (pattern above). An option per 433 MHz zone,
off by default: a closed code arriving while the zone is already closed, or an open code while
it is already open, in a new transmission (not a repeat within `ZSRC_RF_REPEAT_MS`), is reported
as the zone's tamper - a trouble when disarmed and an alarm when armed.

- **Decided 2026-09-25:** liked, as an option for the 433 MHz sensors; "We will leave it alone
  for now."
- **Risks to weigh first:** a missed open transmission turns the next closed code into a false
  tamper (rare at ~36 frames an event, likelier at the edge of range); a sensor that sends a
  periodic heartbeat of its state would read as a tamper every time (none seen on this one -
  check over a night first); after a restart the panel starts each zone closed, so a door left
  open through a restart reads its closing as a tamper once.
- **Where it would go:** `zsrc_rf` (the zone's watch knows its state and its closed code:
  `closed_by`, RF-021), a binding option in the stored blob (a new field id, docs/features/22),
  the tamper page ("This sensor's cover sends its door code again: report that as a tamper"),
  host tests with the patterns above, and an RF requirement in docs/features/25.

## Adding an entry

Copy this outline for a new sensor model:

```
### <Protocol and codes, for example: EV1527 door contact (6 open, 9 closed)>
- **Sold as:** what kind of sensor it is, and how it is listed.
- **Power:** cell, voltage, low-battery threshold if the listing gives one.
- **On the air:** protocol, timing, frames per event, signal strength at a known distance.
- **Messages:** every code or message and what caused it; low battery; heartbeat.
- **Proven:** date, panel, how (console, captures), links to evidence.
- **What RoboAlarms does:** as of a date, and what to set.
- **Open:** what is still unknown.
```

Keep it public-safe: no addresses of real homes, Wi-Fi names or panel IDs (CONTRIBUTING.md
"Conventions"); a sensor's radio address is fine to note (it is on the air for anyone).
