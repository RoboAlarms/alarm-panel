# B024 · Restarting a running Zigbee radio restarts the whole panel (a panic)

**Status (2026-10-04): fixed (a836424), tried on a bench panel: 40 restarts in a row, the network back each time, no memory lost.** Backlog: BL-161 (closed).

## Symptom

On a bench panel (CrowPanel Advanced 7" V1.1, P4 v1.3, a v1 dev build of the Z-Wave TLC work on
top of the Zigbee commit 6da5940), the console's `zigbee retry`, typed while the Zigbee stack was
running, panicked the panel at once; it restarted by itself and came back with the radio ready.

```
Guru Meditation Error: Core  0 panic'ed (Illegal instruction). Exception was unhandled.
MEPC    : 0x4018b01c  RA      : 0x40112aca  ...
MCAUSE  : 0x00000002  MTVAL   : 0x34702773
```

Decoded against the flashed ELF (`riscv32-esp-elf-addr2line`):

| Address | Where |
|---|---|
| MEPC 0x4018b01c | `esp_zigbee_task_queue_post` (the Zigbee library, no line numbers) |
| RA 0x40112aca | `radio_port_restart`, `zigbee_service/src/radio_port.c` (its `esp_zigbee_stop()` call) |
| stack 0x4010c95c | `restart_if_asked` in `zigbee_task`, `zigbee_service/src/zigbee_service.c` |

MTVAL 0x34702773 is the instruction that trapped: `csrr a4, 0x347`, a read of CSR 0x347.

## What reaches it

`restart_if_asked()` stops a running stack with `radio_port_restart()`, which takes the stack's
lock (`zb_lock_any()`) and calls `esp_zigbee_stop()` while holding it. Every restart request
comes this way when the stack is up:
- the console's `zigbee retry` and `zigbee restart`;
- the touchscreen's and the Panel Manager's "Restart the radio" (`UI_ZB_REQ_RESTART`, through
  `zigbee_service_request`);
- `zigbee_service_retry()`;
- the developer tool's `restart` and `retry` (`zigbee_dev.c`).

So, if this reproduces, any of these restarts the whole panel whenever the radio is working. The
bench checks of 2026-10-03 never restarted a running stack: `zigbee retry` ran
there only while the radio was paused or silent, when `s_stack_up` is false and nothing is
stopped.

## Cause

Espressif's prebuilt `esp-zigbee-lib` 2.0.4 (the newest, 2026-08-14) has one esp32p4 build, and
it is made for P4 **v3** silicon. Its `esp_zigbee_task_queue_post()` checks whether it was called
from an interrupt by reading the CLIC's standard threshold and status registers (`csrr a4, 0x347`,
`csrr a5, 0xfb1`). P4 v1.x silicon (every panel here) has neither: ESP-IDF's own
`soc/esp32p4/include/soc/interrupt_reg.h` says its threshold is a memory-mapped register and its
status CSR is 0x346. So the first instruction that touches them traps, every time, whatever the
caller. It is the only function in the image that does (objdump of the whole ELF).

Two callers reached it:
- `esp_zigbee_stop()`, which posts its main-loop exit: every radio restart (`radio_port_restart`);
- `retry_fired` in `radio_port.c`, the commissioning retry after a failed start or formation: a
  radio that fails once at start-up would have panicked the panel the same way.

The library's compat alarms (`esp_zb_scheduler_alarm`) are built on the same post; this firmware
doesn't link them.

### Behind it: starting the stack again in the same boot

Once the stop worked, the bench panel showed three more faults on the way back up:
- **The second start ended the panel.** ESP-IDF's radio Spinel layer
  (`components/openthread/src/spinel/esp_radio_spinel.cpp`) hands its one `SpinelDriver` to
  `Init()` on every `esp_zigbee_init` but never calls its `Deinit()` (ESP-IDF's own OpenThread
  port does, in `esp_openthread_radio_spinel.cpp`). The second `Init()` adds the IID list again,
  overflows it, and OpenThread ends the panel: `Init() at spinel_driver.cpp:82: Failure`, then
  `abort()`.
- **A stop during a transmission ended the panel** (1 restart in 4). The stack's main loop can end
  with a frame still on the air; OpenThread's `RadioSpinel::Sleep()` then refuses (it sleeps only
  from receiving), and esp-zigbee's `ezb_plat_radio_sleep()` aborts on the refusal while
  `esp_zigbee_deinit` takes the network down (`nwk_deinit` -> `mac_pal_disable`).
- **Each restart lost about 1.1 KB of internal RAM** (9 KB per 8 restarts, twice).
  `esp_zigbee_deinit` frees the stack's globals but not the device descriptor the panel registered
  (its endpoint, clusters and attributes, and the Green Power endpoint the stack adds to it):
  nothing in the deinit path calls `af_free_device_desc` (objdump of the library).

The `uart: GPIO 53 is not usable, maybe used by others` warning at every start (and at boot) is
harmless: the radio answers on those pins every time.

## Fix (2026-10-04, a836424)

- `retry_fired` takes the stack's lock and runs the commissioning step itself (the stack queues
  its own work and wakes its loop), trying again after the shortest delay if the lock is held;
  the stack's task stops the retry timer before it tears the stack down.
- On v1.x builds (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`) the linker sends every call of
  `esp_zigbee_task_queue_post` to `__wrap_esp_zigbee_task_queue_post` in `radio_port.c`
  (`-Wl,--wrap`, zigbee_service's CMakeLists.txt): the callback (only `esp_zigbee_stop`'s exit
  flag) runs at once under the stack's recursive lock, then `ezb_tasklet_signal_pending()` wakes
  the main loop. The library's function is no longer in the image (no `csrr 0x347` left).
- v3 builds keep the library's own post.
- Every build with Zigbee also wraps two of OpenThread's `SpinelDriver` methods (by their C++
  names) and `esp_radio_spinel_sleep` (`radio_port.c`, the same CMakeLists.txt):
  - `Init()` on a driver that was started before first calls its `Deinit()` and sets its
    reset-failure callback again (which the reset clears; `SetCoprocessorResetFailureCallback` is
    wrapped only to remember it);
  - while `stack_task` is in `esp_zigbee_deinit`, a refused sleep first takes the radio back to
    receiving (`esp_radio_spinel_receive` on the network's channel, what the MAC does after every
    frame) and asks again; OpenThread then logs the frame's late answer as an unexpected Spinel
    transaction.
- `stack_task` frees the panel's device descriptor with `ezb_af_free_device_desc` after
  `esp_zigbee_deinit` (recursive: endpoints, clusters, attributes).

Tried on the bench panel (P4 v1.3, the v1 dev build): `zigbee restart` 40 times with the radio
up, in runs of 2, 14, 8 and 8 a few seconds apart; the network came back each time in about a
second on the same channel and PAN, the stop during a transmission was met twice and handled, and
internal RAM stayed within 150 bytes over 16 restarts (198,583 to 198,435 bytes free).

Check at each esp-zigbee-lib or ESP-IDF update: remove the task-queue wrap once the library runs
on v1.x, and the Spinel wraps once `esp_radio_spinel_rcp_deinit` resets the driver itself (a
wrapped C++ name that no longer exists fails the link, so a rename can't slip through).

## How to reproduce

A panel with the H2 running `firmware/radios/h2_rcp` and `zigbee status` saying `ready=1`; type
`zigbee restart` (or tap "Restart the radio"). Capture the panic over the console
(`python tools/panel_console.py -p <PORT> --seconds 30 "zigbee restart"`) and decode MEPC, RA and
the stack with the ELF of the image the panel runs.
