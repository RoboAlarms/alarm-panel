# Recorded Z-Wave exchanges

Every `*.zwtrace` here is replayed by `test_every_recording_replays` in `../test_zwz_replay.c`
(suite `zwz_replay`, docs/features/36 "Test and acceptance"): the recording plays the ZAC93, the
panel's own Z-Wave (`zwz_panel`, as the firmware's `zwave_service` runs it) plays the panel, and
every write the stack makes must be the one recorded. One bench session with a real module becomes
a regression test that runs in CI forever, with no hardware.

## What the test asks of every recording
- It went through `tools/zwtrace_scrub.py` (its mark is on line 2) and lost no entries.
- It plays out from its first start-up (the panel's lone NAK of serial-api.md §5 step 1): every
  write as recorded - a callback ID excepted, which follows the stack's own - and every module
  entry delivered at its recorded time after the write before it. The installer's actions come
  where the recording shows the writes they cause: "Restart the radio" (a start-up's NAK while the
  radio works), the add and remove windows opened (`4A`/`4B` any) and stopped (`4A`/`4B` stop).
- At the end the pages show the radio working with what the module said about itself in the
  recording: firmware, SDK, chip, region, home ID, the panel's node ID.

A mismatch fails the test with the entry's line and both frames in hex:

```
tx #46 (line 50, 1500 ms): the stack wrote
  010c0003010100055e9f6c568f81
where the recording has
  010d0003010100065e9f6c568f981b
```

## From a bench recording to a fixture
1. On the bench, with the maintainers' go (docs/features/36 Z9): a development build with Z-Wave, the
   radio placed, every partition disarmed, then
   `python tools/panelctl.py zwave check --device <alias>` - without `--sensor`: a sensor pass
   adds a device, whose Security 2 exchange the replay cannot follow (below). It restarts the
   radio, opens the add and remove windows for a few seconds each and writes
   `build/zwave-check-<date>/check.zwtrace` (the whole check), `startup.zwtrace` (the restart
   alone) and `report.md`. A trace of something else:
   `python tools/panelctl.py zwave trace --device <alias> --follow --output <file>` around it.
2. Scrub it into this folder:
   `python tools/zwtrace_scrub.py build/zwave-check-<date>/check.zwtrace components/core/zwave_zones/test/traces/zac93_<firmware>_check.zwtrace`
   (for example `zac93_1.50_check.zwtrace`). The raw recording carries the network's home ID:
   it stays in `build/`, never in the repository.
3. Read the scrubbed file: nothing but entries, `lost` lines and the kept comments (the tool, the
   module's firmware, the date).
4. Build and run the host tests (`pwsh tools/host_test.ps1`). The new file is found at the next
   build. If it does not play out, the message names the first write where the stack and the real
   module parted: a difference the fake ZAC93 never showed. Find out which side is wrong, fix the
   stack (or the fake, when the real module is right), and note what was found in
   docs/features/36 (serial-api.md where the module differs from the document).
5. Add a test beside `test_fixture_check_windows` that names what the recording proves - the real
   module's identity and function mask, its timings, how it answers a stopped window - tagged
   with the requirement (ZW-004, ZW-005, ZW-016...), and a row to the table below. Real
   recordings join under their own names; the synthetic ones stay, since their tests name them
   and they show what a bench rarely does (line noise, a watchdog restart). Retire one only
   together with its test, when a real recording proves the same thing.

## What the replay needs of a recording
- **A start-up in it.** `zwave check` begins with one; for `zwave trace`, start it before the
  radio restarts (or use `--all` right after the panel starts). Anything before the first
  start-up is skipped.
- **No devices yet.** The replay's panel starts with nothing stored, as a fresh one: a module
  whose node list holds only the panel is what it expects. A recording of a network with devices
  would also need the panel's stored network (its node cache and keys), which the recording does
  not carry: not supported yet.
- **No device traffic to match byte for byte.** Security 2 frames depend on the network's keys and
  random nonces, which never cross the wire: sensor traffic is proven against the fake devices
  (`zwave_host/test/fake_zdev.c`), not replayed. The scrubber replaces S2 public keys anyway.
- **The actions it knows.** Restart, add, remove, stop (`act()` in `../test_zwz_replay.c`); a
  recording with others (a region change, a network reset, a ping) needs `act()` to learn them.
- **Nothing chosen.** No region picked and no reset line: the replay's panel has neither. A panel
  with the reset line wired records a SerialAPIStarted before its NAK; the replay's stack takes it
  while it listens for the bootloader, as the start-up allows.
- **Nothing lost.** A `lost` line means the developer link missed entries: record it again.
- **Its own timing.** A write the stack makes by itself may come later than recorded (the recorded
  panel was a little quicker) within 40 s; an installer's action comes 10 ms after the recorded
  write was due.

## The recordings here

The four scenario recordings and the deliberate mismatch are **synthetic**: recorded by `../test_zwz_replay.c` from the fake ZAC93
(`../../../zwave_host/test/fake_zac93.c`: a ZAC93 on SDK 7.22.1, firmware 1.50, region USA) through
`../zwtrace_record.c`, which writes what the panel's zw_link task would, then scrubbed.

| File | What it proves |
|---|---|
| `zac93_sdk_7_18_3_startup.zwtrace` | **Physical ZAC93**, SDK 7.18.3 / firmware 1.10: real startup answers, reset-line noise and a cut-off frame, with its home ID scrubbed; SUC was already set before this recording |
| `zac93_sdk_7_18_3_windows.zwtrace` | **Physical ZAC93**, SDK 7.18.3: software restart and add/remove Listening/Stop windows on the fixed firmware, with its home ID scrubbed. The later GPIO pulse produced no UART activity and is not proven by this recording. |
| `startup.zwtrace` | the panel's first start against a factory module: serial-api.md §5 end to end, the module's facts on the pages, the keys made once and bound to the home ID (ZW-004) |
| `startup_line_noise.zwtrace` | the same through a noisy line: junk bytes (text), a frame the byte timeout cut off, an answer damaged on the way (NAKed and sent again) (ZW-004) |
| `radio_restart.zwtrace` | the module restarting by itself (its watchdog) while the link is up: steps 13-19 again, "restarted by itself" logged once (ZW-004) |
| `check_windows.zwtrace` | `zwave check` with no device, recorded from its first step as panelctl does: the radio restarted from the pages, the add window listening then stopped, the remove window the same; its callback IDs were a running panel's (ZW-005, ZW-016) |
| `bad/startup_node_info_differs.zwtrace` | deliberately wrong, replayed by `test_a_mismatch_is_reported` only: `startup.zwtrace` with its SetApplicationNodeInformation listing Security 0 (`98`) too, as a panel built with S0 would send it (checksum recomputed) - the replay must report it |

To make the synthetic ones again (after a change to the stack's start-up the replay reports as a
mismatch, once it is checked against serial-api.md):
`ZWZ_RECORD_DIR=<folder> build/host/test_zwz_replay` (PowerShell: `$env:ZWZ_RECORD_DIR=...`)
writes the raw recordings; scrub each into this folder with `tools/zwtrace_scrub.py`, and derive
the bad one from the new `startup.zwtrace` as described in its row.

## Format
`zwtrace 1`, then `#` comments, `lost <n>` lines and one line per entry, `<seq> <t_ms> <dir>
<hex>`: `rx` a frame from the module (as it came), `tx` a write of the panel, `crx`/`ctx` an ACK,
NAK or CAN received/sent (a `ctx` right after an `rx` is the link's answer to it), `text` bytes
outside frames. `# panel restarted` marks where the panel rebooted (the replay restarts its stack
there with what it had stored). docs/features/30 "Z-Wave" has the developer link's side,
`../zwtrace.h` the reader (up to 8192 entries and 256 KB of bytes in one file: a `zwave check`
is a few hundred).
