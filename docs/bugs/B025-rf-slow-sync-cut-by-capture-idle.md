# B025 · A slow 433 MHz sensor's sync outlasts the receiver's idle, and the panel stops hearing it

**Status (2026-10-04): fixed in code (f2dc2b2), host-tested; not yet tried on a panel.**
Found by the mock devices (docs/features/25 "Mock devices"), not on the air. Backlog: BL-163.

## Symptom

None seen on a panel yet. On the PC, a mock 12 V EV1527 contact (T 500 us fresh) whose cell has
sagged to 5 V - its RC oscillator about 31 % slower (docs/features/25 "What goes wrong on the
air", row 11), T about 655 us - opens its door and the zone doesn't move: no frame of the burst
decodes. The decoder's own tests said every T up to 800 us decodes
(`test_every_transmitter_speed_decodes`), because they fed it the pulses straight, without the
receiver in between.

## Cause

The capture ends a reception when the line has been quiet for `RF_CAPTURE_IDLE_US` (rf_rx,
20 ms), and the decoder is then told "gap" instead of the low's length. EV1527's sync is a 1 T
mark and a 31 T space: at T above 645 us the space is longer than 20 ms, so every sync becomes a
gap and no frame can start. The decoder takes T up to `RF_EV1527_UNIT_MAX_US` (800 us), so it
accepted on paper what the receiver could never hand it. HT12E's pilot (36 T) was cut from T 556
us the same way.

Who it would hit: a sensor that is slow from the start, or one that slows as its cell sags - the
12 V A23 cells of many cheap contacts and remotes (docs/features/25, row 11), just when its
low-battery code matters. A T of 500-550 us is common for the slower EV1527 sensors (a Tasmota
capture with Sync 22858 / Low 754 / High 2368 is T 737 us); the mock profile assumes 500 us
fresh.

## Fix

- `RF_RECEPTION_IDLE_US` (rf_decode.h): the reception's idle, now **30 ms**, with the rule that
  every protocol's longest space at its slowest T must fit (EV1527 at 800 us: 24.8 ms; HT12E at
  800 us: 28.8 ms).
- `RF_CAPTURE_IDLE_US` (rf_capture.h) is 30 ms; `rf_service.c` asserts the two are equal, and
  `rf_capture.c` asserts it fits the RMT's 15-bit idle counter at 1 us ticks (32.767 ms on the
  P4, `RMT_LL_MAX_IDLE_VALUE`).
- Cost: a transmission is known to have ended 10 ms later; two bursts 20-30 ms apart are now one
  reception (the decoder's framing is unchanged by it).

## Tests

- `test_every_protocol_at_its_slowest_fits_a_reception` (rf_decode): every protocol at its
  table's slowest T, through a receiver that turns a low of the idle's length into a gap. Failed
  on EV1527 at 20 ms; passes at 30 ms.
- `test_a_sagging_cell_is_still_heard` (zone_hub, test_rf_devices.c): the mock contact on a low
  cell opens its zone and its low-battery code reaches the zone. Failed at 20 ms; passes.

## To verify on a panel

Nothing should change for the sensors on the bench (their syncs are 9-13 ms). With the dev
build on a panel with a receiver: a contact's open and close still arrive (`rf status`: receptions
and frames counting); a remote held down is still one transmission. A slow sensor to prove the
fix itself would be an EV1527 transmitter run from a bench supply at 5 V, or a WL102-341 driven
with `rf_ev1527_encode()` pulses at T 700 (BL-094).
