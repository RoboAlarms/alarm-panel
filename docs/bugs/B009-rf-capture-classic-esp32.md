# B009 - The 433 MHz capture failed ten times a second on the classic ESP32

**Status: fixed and running on the 4" board (E32R40T, COM3), 2026-09-23.** Found on its first boot
with this project's firmware. The P4's capture is configured exactly as before.

## Symptom
Right after start-up, forever, every ~100 ms:

```
E (1537) rmt: rmt_receive(355): partial receive not supported
E (00:00:00.825) rf_capture: rf_capture_start(100): receive
```

and, once that was fixed, one line at boot:

```
E (00:00:00.967) gpio: gpio_pulldown_en(127): GPIO number error (input-only pad has no internal PD)
```

## Cause
- `components/drivers/rf_rx/src/rf_capture.c` always asked the RMT driver for partial receive
  (`en_partial_rx`), which needs ping-pong receive (`SOC_RMT_SUPPORT_RX_PINGPONG`): the P4 has
  it, the classic ESP32 doesn't, so every `rmt_receive()` was refused.
- `components/services/rf_service/src/rf_service.c` tried to start the capture again on every
  100 ms poll while no receiver was found, so the refusal was logged ten times a second.
- The capture pulled its data pin down so an unwired pin reads quiet; the E32R40T's RF data pin
  is IO35, one of the ESP32's input-only pins, which have no internal pulls.

## Fix
- Without ping-pong receive: no partial receive, and a 256-symbol (4-block) channel that holds a
  whole burst of about ten EV1527 frames; with it (the P4) unchanged: partial receive, 96
  symbols.
- A capture that won't start is tried again with the next probe (30 s), and at once on
  `rf retry`.
- The pull-down only on pins that have one (`GPIO_IS_VALID_OUTPUT_GPIO`).

## Verify
Boot log clean apart from the expected display/touch/audio "not supported" lines; `rf` says
"No receiver yet: no CC1101, GPIO 35 quiet" with the capture running. Still to check with a
receiver module on IO35: frames decode with whole-burst receptions (`rf capture 3000`,
`rf raw on`). The P4 v1 dev build compiles with the change.
