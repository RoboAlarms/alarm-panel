# B020: a door contact can be left showing closed after a fast open with a bounce

**Status: fixed in the panel (a836424), host-tested; the bench check with a bouncing contact is
left.** The sensor doesn't always send its last change; the panel now reads a Zigbee sensor's state
again after a burst (ZB-028), and the 433 MHz source no longer takes a new code for a repeat of an
older one (RF-023). Backlog: BL-155.

## Symptom
On the bench panel, a Zigbee door contact opened fast, with the magnet bouncing past the reed
switch for a split second as it moves away, can leave the panel showing the door closed while it
is open. Opening and closing it quickly can leave the wrong state too. The same is suspected of
other kinds of sensor, not only Zigbee.

## Evidence
The first observation, then a bench trace (below). The sensor is a SONOFF
SNZB-04PR2 (SenseGuard DW Gen2), a sleeping end device with a plain IAS Zone contact, Poll Control
(0x0020) and two vendor clusters (FC57, FC11): its signature is in
`components/core/zigbee_zones/test/signatures/sonoff_snzb_04pr2.json` and testing.md's table. Poll
Control means the panel can ask for its zoneStatus after a burst, answered at its next check-in.

**Bench trace, 2026-10-03** (`zigbee trace on` for five minutes while the magnet was moved;
the panel ran 6da5940's content). Every IAS Zone Status Change Notification the sensor sent arrived
(signal -35 to -41 dBm, LQI 170, radio events dropped 0), in order, and the zone followed each one:
at the end the zone, the device's page and the last notification all said closed, matching the
magnet. Changes 30 to 100 ms apart (the bounce) arrived as separate notifications. But the sensor
left out changes:
- open at 84.2 s, then open again at 152.4 s with no closed between, though the magnet had been closed
  in that time: the panel showed open while the door was closed;
- open at 173.1 s, 192.9 s and 195.3 s with no closed between them: two closes never sent;
- before the trace, the panel showed closed with the magnet away: the same failure the other way.
So the wrong state comes from the sensor not reporting its final change after a bounce, not from
the panel dropping or reordering anything. Others report the same with this model on firmware
4097 (1.0.1, ours too): a magnet that slides past makes it report several times and stay in the
wrong state until the door moves again
([Home Assistant community](https://community.home-assistant.io/t/sonoff-snzb-04pr2-missed-updates-multiple-triggers-and-wrong-contact-state-anyone-else/1020489)).
Not known yet: whether the sensor's own zoneStatus attribute is right when its notifications were
wrong (no console command reads it yet). If it is, the panel can read it after a burst; if not, no
panel can know, and the device table should say so.

## Why it matters
A zone that reads closed while its door is open hides the opening from the panel: the partition
reads ready, arming doesn't warn, and while armed the next opening may go unseen if the panel
thinks nothing changed. The last state the panel holds must be the state the sensor last sent,
and an opening that lasted only a moment while armed must still count.

## What we found
- **The sensor** (the SNZB-04PR2 above) sometimes leaves out its last change after a bounce. No
  panel can follow a change it is never sent; the one thing it can do is ask the sensor again.
- **Zigbee on the way in:** nothing dropped or reordered. The radio port, `zbz_panel` and
  `zsrc_zigbee` keep frames in order, the radio queue counts what it drops (none on the bench), and
  a status change is told to the engine at once - no "same state" filter against a stale copy, no
  rate limit. A notification and a zoneStatus attribute report go the same way.
- **The engine** acts on a zone's FAULT the moment it arrives (only the start-up settle waits), so
  an opening that lasted 80 ms on an armed partition is an alarm, and a later RESTORE doesn't undo
  it (`test_a_momentary_opening_while_armed_counts`).
- **433 MHz had a bug of its own:** a transmitter's codes within `ZSRC_RF_REPEAT_MS` (500 ms) of
  an earlier transmission of the same code counted as that transmission's repeats. A door opened,
  shut and opened again in that time sends open, closed, open: the second open was taken for a
  repeat of the first and dropped, and the zone stayed closed with the door open. Fixed (RF-023).
- **Z-Wave:** each Notification or Binary Sensor report is a change told in order; a report sent
  twice leaves the zone where it is; S2's sequence-number duplicates are already dropped
  (ZW-007). Nothing to fix; a replay test now proves it (`test_a_bounce_ends_on_the_last_report`).
- **Wired inputs** keep the wired loop response (`ZSRC_WIRED_RESPONSE_MS`, 350 ms, ZONE-004): a
  change shorter than that is noise by design. **Home Assistant** zones follow the
  entity's state events in order.
- **What others do.** Zigbee2MQTT (zigbee-herdsman) follows every notification and drops a frame
  whose ZCL sequence number repeats the last one from that device and endpoint within 10 s. ZHA
  (zigpy) reads zoneStatus only at start-up. Z-Wave JS has no duplicate filter for Supervision
  sessions; it drops S2 duplicates by sequence number, as ours does. None of them reads the state
  again after a burst; the panel now does.

## Fix
- **Zigbee (ZB-028):** two changes of a sensor's alarm bits within `ZSRC_ZB_BURST_MS` (3 s) are a
  burst. Once the sensor has been quiet for `ZSRC_ZB_REREAD_MS` (1 s), the panel reads its
  zoneStatus attribute and the answer wins. A sleepy sensor's parent holds the read for its next
  poll; with no answer in `ZSRC_ZB_REREAD_WAIT_MS` (10 s) the panel asks again at its next frame
  (its check-in), twice in all. Every notification is still followed at once, so nothing is
  delayed. No duplicate filter by ZCL sequence number: a repeated status changes nothing anyway,
  and a filter that dropped a real change (a sensor that reuses its numbers after a rejoin) would
  be worse than a repeat.
- **433 MHz (RF-023):** a frame continues a transmission only while its transmitter has sent no
  other confirmed frame since; a bouncing magnet leaves the zone where the sensor's last code says.
- **Z-Wave:** none needed.
- Tests: `test_zigbee.c` (`test_a_burst_is_read_again_and_the_answer_wins`,
  `test_a_plain_change_is_not_read_again`, `test_an_unanswered_reread_is_asked_at_the_next_frame`,
  `test_a_momentary_opening_while_armed_counts`), `test_rf.c`
  (`test_open_shut_open_at_once_ends_open`, `test_a_bounce_ends_on_the_last_code`,
  `test_a_lone_other_code_does_not_split_a_transmission`), `test_zsrc_zwave.c`
  (`test_a_bounce_ends_on_the_last_report`).

## How to verify
Host tests: done (above). Left, on the bench: the SNZB-04PR2 moved fast past its magnet many times
with `zigbee trace on`; after each burst the trace shows the read (`read_attribute`) and its answer,
and the panel's state matches the door. If the sensor's own zoneStatus is wrong too, the panel can't
know better: then the device table says so for this model.
