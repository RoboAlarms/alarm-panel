# Recorded 433 MHz captures

Every `*.txt` here is decoded by `test_recorded_captures` in `../test_rf_decode.c` (suite
`rf_decode`). They are the regression tests the air writes: once a real sensor's capture is
here, no change to the decoder can stop it decoding, or make it decode as something else.

## Adding a capture from a real sensor
1. Record it: the RoboAlarms Flasher's 433 MHz page, **Record 3 seconds** (saved in its
   Captures folder), or `rf capture 3000` on the panel's console (copy the lines it prints).
   Trigger the sensor while it records; do it twice for a door contact (open, then close).
2. Name the file `<protocol>_<what>_<receiver>.txt`, for example `ev1527_door_contact_rx470.txt`
   or `ht6p20b_remote_cc1101.txt`.
3. Add one `# expect:` line per code the sensor sent: the protocol name as the decoder spells it
   (`EV1527`, `HT6P20B`, `HT12E`, `KeeLoq`, `arctech`), the device and the data in hex, as
   `rf raw on` logs them (`EV1527 A2B3C code 5` becomes `# expect: EV1527 A2B3C 5`).
   A capture of an empty band is `# expect: nothing`.
4. Keep the Flasher's `#` header lines (when, receiver, firmware): they say where it came from.
5. Re-run CMake (the list of files is read when it configures), then the host tests.

## What the test asks
- Every expected code decodes at least twice: a transmission, as the zone logic counts it
  (RF-014).
- No other code decodes twice: a stray frame from noise may decode once; twice would be a
  phantom sensor.

## Format
`# key: value` header lines, then rows of pulses: `H350` is 350 us high, `L10850` 10850 us low;
`H0` / `L0` is the line going quiet (the capture's gap). Rows may have any number of pulses.

## The captures here
| File | Source | What |
|---|---|---|
| `synthetic_ev1527_door.txt` | **synthetic** (a script's model of a receiver, not the bench) | an EV1527 door contact opening then closing, with static between |
| `synthetic_static_only.txt` | **synthetic** | an empty band's static: nothing may decode twice |

Replace or join the synthetic ones with real recordings as the bench produces them
(docs/features/25, "First bench").
