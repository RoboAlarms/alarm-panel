# zigbee_host

**Tier:** core (portable C11) · **Features:** [20 ESP32-H2 sensor radio](../../../docs/features/20-ESP32-H2-Sensor-Radio/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `zb_bringup`)

The panel's side of the Zigbee sensor radio. Today it holds one thing: the bring-up policy, a
state machine that asks the ESP32-H2 which firmware it runs and, when that is not the image this
firmware carries, asks it to reboot into its ROM bootloader, writes the image a step at a time,
restarts it and checks the version again. It waits while any partition is armed (writing the
radio takes every wireless sensor off the air), bounds its retries, and says so when a socket is
silent or a module runs firmware it cannot ask to reboot. It is a tested policy, **not** a
working updater: the probe, transport, image writer and restart come from the platform through
`zb_boot_port_t`, and `components/services/zigbee_service` (`src/radio_port.c`) gives it a
real probe but no image or flash writer yet, so on a panel it only finds the radio and hands it
to the Zigbee stack. Mapping Zigbee sensors to zones is `zone_hub`'s `zsrc_zigbee.c`, not this
component.

## Public API
| Header | What it offers |
|---|---|
| `include/zigbee_host/zb_bringup.h` | `zb_boot_port_t` (probe, to_bootloader, sync, flash_step, flash_reset, restart), `zb_boot_t`, `zb_boot_state_t`, `zb_probe_t`; `zb_boot_init()`, `zb_boot_start()`, `zb_boot_tick()`, `zb_boot_next()`, `zb_boot_lost()`, `zb_boot_state()`, `zb_boot_ready()`, `zb_boot_words()`; the `ZB_BOOT_*` timings and retry limits |

## Depends on
- Project components: none
- ESP-IDF: none (portable); `idf_component.yml` asks for ESP-IDF 6.1 or later

## Options
None in this component. `CONFIG_ALARM_ZSRC_ZIGBEE` (`components/services/zigbee_service/Kconfig`,
default n, on in the P4's defaults) builds the service that uses it.

## Rules
- The caller must prevent arming throughout a real update. `may_update` is checked again before
  each destructive step, and withdrawing it pauses the operation where it is (ZB-001).
- Never enter the loader or call a writer without an image. A transfer counts only after a
  restart and a matching version probe; failed attempts are bounded.
- The socket's RST and BOOT reach no P4 pin, so the only way into the bootloader is asking the
  running radio over the UART; the module's IO12/IO14 sit on the touch I2C bus
  (`board_pins.h`).
- The tests use a fake radio already able to take an image; they prove the policy, not that a
  first install over this wire works on real hardware.
- Portable rules (CONTRIBUTING.md): C11, no heap, no globals, no RTOS or ESP-IDF headers; time comes
  in as `uint64_t` ms; the caller owns the state and its serialisation.

## Tests
Run all with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`;
this suite alone with `ctest --test-dir build/host -R zb_bringup`. The cases are tagged ZB-001.
- A radio already running the carried image is left alone; an older one is updated over the
  same wire; a module already in its bootloader can be written.
- Foreign firmware asks for the module's own buttons; an empty socket says so; a slow radio is
  given time to answer.
- Arming stops every destructive phase; a failed write is retried, then abandoned; a failed
  restart or a wrong version afterwards is not reported as an update and does not loop.
- Progress while writing; a radio that goes away is brought up again; a build with no image
  takes what is there and never writes; nothing happens before it is started or without a port.
Nothing has been tested on a real H2 yet (docs/features/20).
