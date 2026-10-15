# B027: the Flasher's "Check it" never finishes on a Zigbee module

**Status: fixed and proven on a Zigbee module 2026-10-04** (see "On the module"), through the
Flasher's own engine; the exe's button in a user's hands is part of BL-165. Every serial write
the Flasher makes now gives up after 2 s, and its "is a panel here" check takes a write nobody
reads as "no panel".

## Symptom
In the 0.7.0 Flasher, Advanced tools > Zigbee module, with the module (ESP32-H2, out of its socket)
on its own USB-C port: "Check it" showed "Something from Espressif on COMn" and "Looking at it…"
and stayed there. "Install the radio software" goes through the same check, so the module could
not be installed either.

## Evidence
- The Flasher's `Controller.radio_read` run on the module's port from a script, with
  `faulthandler` set to dump the stacks after 20 s: it never returned, and the stack ended in
  pyserial's `serialwin32.py` `write` (line 321, waiting for the write to complete), called from
  `probe.py` `probe`'s first write, `s.write(b"\r")`.
- That probe is the check for a RoboAlarms panel's console that `radio_read` and the `radio` job
  run before anything else (FLS-030: a port where a panel answers is refused). The port was opened
  with no write timeout, so pyserial on Windows waits for ever.
- The module ran an older build of our radio software (`roboalarms_h2_rcp`, a dirty dev build).
  It speaks Spinel on its UART pins and never reads its USB Serial/JTAG input.

## Cause
A write to the module's USB Serial/JTAG port is never taken while its app runs: the app doesn't
read it, so the chip's receive buffer stays full and the USB host waits. (That the buffer was
already full from an earlier attempt is inferred, not shown; the stack shows only that the first
one-byte write never completed.) The Flasher opened every port with `write_timeout=None`, and the
probe was the only step before esptool, which sets its own timeouts and resets the chip into its
loader, which does read.

## Fix
- `tools/flasher/roboflash/serialio.py`: `open_quiet` gives every port `WRITE_TIMEOUT` (2 s);
  `is_write_timeout` recognises pyserial's `SerialTimeoutException`, and `explain` turns one into
  "Nothing on COMn takes what the Flasher sends." with "Unplug the board, plug it back in and try
  again."
- `tools/flasher/roboflash/probe.py`: a write timeout in `probe` means no RoboAlarms console
  reads the port; the result is "nothing answered", with no error, so `radio_read` and the
  `radio` job carry on to the chip.
- `tools/flasher/roboflash/mock.py`: the mock's Zigbee module stalls writes as the real one does,
  and a write to it without a write timeout fails the test (it would hang).

Tests: `tools/tests/test_flasher_panel.py` checks that `open_quiet` sets the write timeout, and
`test_a_port_that_takes_no_writes_does_not_answer` runs `probe` on the mock module and on a port
raising pyserial's real `SerialTimeoutException` (nothing answered, no error) and checks the
explanation's words. `test_flasher_radio.py`'s identify, read and install tests now go through the
stalling mock module.

## On the module
Board 3's Zigbee module, out of its socket, on its own USB-C port, 2026-10-04, with the fix:
- `radio_read` through the Flasher's engine returned in 3.5 s: an ESP32-H2 (v0.1) running our
  radio software, older than the bundle's.
- The Flasher's `radio` job (the "Install the radio software" button's job) with the 0.7.0
  bundle: erase, write the three parts (the app 284,224 bytes), each checked by MD5, restart.
- Read back: `roboalarms_h2_rcp` 1.0.0, the same as the bundle's. A write to the port then stalled
  again, so the app runs, not the loader.

Still owed (BL-165): the same from the rebuilt exe's page, and the seated module answering
`zigbee status` on the panel.
