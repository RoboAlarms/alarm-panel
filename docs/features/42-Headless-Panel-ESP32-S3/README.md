# 42 · Headless panel (ESP32-S3-DevKitC-1)

**Status:** In progress (BL-222): the board, its firmware and phone setup built; on the owner's
development board since 2026-10-21 as the master of the 7" and 10.1" (both its keypads, arming and
disarming through it tried); a keypad's own wired terminals as the master's zones (LINK-055) built
and host-tested · **Milestone:** M7 · **Depends on:** 11, 22, 31, 33

## Goal
An alarm panel with no screen, small and cheap enough to replace the panel in a cupboard: the
alarm itself (engine, event log, zones, sirens, radios, integrations), run from the Panel Manager
on a phone or computer and from keypads of the panel link (the 7" and 10.1" touchscreens). Like a
panel with a screen it can also be a keypad of another master, and then what it gives that master
is its own hardware: its wired terminals become the master's zone inputs.

## Reference behaviour
Many alarm systems put the control board in a cupboard and the keypads on the walls. The board has
terminals for wired zones and sirens, takes add-on radios, and is set up by an installer with a
keypad or an app; the household never sees it. A second board can extend the first with more
inputs. Setting a new board up from a phone - join the board's own Wi-Fi, prove you hold its setup
label, choose the home Wi-Fi - is how many connected devices without a screen start.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| HDL-001 | A firmware for the ESP32-S3-DevKitC-1 N16R8 (16 MB flash, 8 MB octal PSRAM) builds from the shared components with no touch UI, and its image fills at most 85 % of its update slot (BRD-012) | the build (51 %), `tools/check_image_size.py` |
| HDL-002 | The board is described in data: its module's flash, PSRAM, USB and UART0 pins and its strapping pins are never offered; eight wired terminals (IO4-7, IO15-18); a CC1101 or receiver on SPI2 (IO10-14) beside a microSD module (CS IO21); a clock module on IO8/IO9; a Z-Wave radio on UART1 (IO1/IO2, header J3); IO38-42 and IO47 for a person's own parts | `components/boards/board_esp32s3_devkitc/test` (BRD-008) |
| HDL-003 | Its console is its USB port (USB-Serial/JTAG), the port people plug in | on the board |
| HDL-004 | A new board opens its setup Wi-Fi (WPA2, the fixed password Robo1234: owner, 2026-10-21; only while in setup or recovery; setup's pages over plain HTTP at http://192.168.4.1 on that Wi-Fi only, every name it looks up answered with the panel and every other address redirected there) and only the setup service answers until a phone on that Wi-Fi claims it (no setup code: owner, 2026-10-21); installer access (required; the household's may follow later), a recovery key and the home Wi-Fi (kept only once confirmed from the home network within two minutes) follow; then the setup Wi-Fi closes and the Panel Manager runs on the home network (docs/features/31 headless-setup.md, approved 2026-10-18) | `web_setup_flow` host tests, `panel_pages.test.cjs` ("headless setup"); on the board from an iPhone 2026-10-21 |
| HDL-010 | Setup ends in the phone's pop-up (the board confirms the home Wi-Fi itself and shows its new address), or over USB from the Flasher (FLS-036) | `panel_pages.test.cjs`, `test_flasher_headless.py`; on the board from an iPhone 2026-10-21 (USB from the Flasher open) |
| HDL-005 | BOOT held five seconds and let go after a normal start opens recovery for five minutes: the setup Wi-Fi again, sign-in, a lost browser password replaced with the recovery key; recovery never disarms or silences | `web_setup_flow` host tests; on the board (open) |
| HDL-006 | It is a master of the panel link over the home network (no ESP-NOW: no co-processor), with the 7" and 10.1" as its keypads | on the bench 2026-10-21: both paired, arm from one, disarm from the other |
| HDL-007 | It can be a keypad of another master; with no screen to need, its update self-test waits for the link only | the build; on the board (open) |
| HDL-009 | Automations are set up and run without a screen: the Panel Manager's Automations page, its devices from Home Assistant (paired in the Panel Manager) or Z-Wave; an arming automation's warning and Put off on the link's keypads (LINK-056) and the Panel Manager's Overview | the automation service on the board (`automations`: running, editable), `test_plink_link.c`; a saved automation on the board open |
| HDL-008 | As a keypad (with a screen or without), its wired terminals are read and offered to the master as zone inputs (LINK-055, zone source "Keypad input") | `test_plink_link.c`, `test_plink_msg.c`, `test_zsrc_keypad.c`; on the bench (open) |
| HDL-011 | Internal RAM stays the reserve with the S3's own Wi-Fi (docs/bugs/B001): the radio keeps 8 internal receive buffers, a block-ack window of 6 and its code in flash; its TX buffers, lwIP, mbedTLS and mDNS use PSRAM | on the board 2026-10-21, a master on the home Wi-Fi: internal free 33 KB -> 72 KB, largest block 15 KB -> 31 KB |

## Design
- `components/boards/board_esp32s3_devkitc`: the description (`board_desc.c`, from `board_pins.h`)
  and the board contract with no screen, touch, speaker or battery (those answer
  `ESP_ERR_NOT_SUPPORTED`); `board_boot_button_down()` for recovery; the add-on I2C bus for a clock.
- `firmware/esp32s3_devkitc`: the manifest (`sdkconfig.defaults`: the S3, octal PSRAM, signed
  updates, the console on USB-Serial/JTAG, Z-Wave on UART1, no UI, no Zigbee socket, the panel
  link, the Panel Manager, MQTT, Home Assistant, notifications, SD over SPI, the clock module) and
  `main/app_main.c`: the shared start-up, the Panel Manager without a screen, the BOOT button, the
  keypad role's health mask (the link).
- `partitions.csv`: the P4's slots (4.5 MB each) without the co-processor's.
- Setup and recovery: `components/services/panel_web/src/web_setup.c` (the adapter of
  `web_setup_flow.c`), the setup Wi-Fi in `net_wifi` (`net_wifi_setup_ap`), the pages in
  `panel.js` ("Setting up a panel without a screen"). The setup Wi-Fi's password is Robo1234 on every board
  (owner, 2026-10-21); the setup code is made at the first start and kept in NVS `websetup`; `web setup` on the service console
  prints them with the certificate's fingerprint.
- A keypad's inputs (LINK-055): `plink_io_svc.c` reads the keypad's terminals with the wired source
  (`zsrc_wired`, its 350 ms debounce) and sends IO_STATE; the master's `zsrc_keypad` turns each
  keypad's terminals into sensors with the address `device << 8 | gpio`.

## Interfaces and data
- NVS `websetup`: `rec` (the setup record, 8 bytes: version 1, stage, flags, revision u32 LE),
  `code` (setup code; `ap`, a random setup Wi-Fi password of the first builds, is erased), `rkey` (recovery key: 16-byte salt, SHA-256).
- `GET /api/v1/setup` and `POST /api/v1/setup {op: claim|accounts|key|key-saved|network|cancel|
  confirm|reset}` (web_setup.h).
- The panel link's IO_STATE (0x3D, PLINK_CAP_IO 0x0010; docs/features/11 LINK-055, frozen in
  `plink_io_fixture_v1.h`).

## Test and acceptance
- Host: the board's description, the setup controller (`web_setup_flow`), the link's IO_STATE and
  `zsrc_keypad`; the Panel Manager's setup pages against the mock (`MOCK_SETUP=...`).
- On the board: phone setup from a new board (every step, a wrong code, a wrong Wi-Fi password, the
  confirmation from the home network, a confirmation not made in time), recovery with BOOT, the
  Panel Manager after setup; as a master: keypads, wired zones, outputs, the clock and SD modules,
  the 433 MHz radio, Z-Wave; as a keypad: its terminals as the master's zones (open and close one,
  unplug the keypad: CHECK).

## Open questions
- The setup label: printed where, by whom (the Flasher could print it after installing), and how it
  survives a factory reset (today the values are made again after an erase).
- Certificate trust on the phone: the browser warns about the panel's own certificate; the label's
  fingerprint is the check today (headless-setup.md's open point).
- A headless keypad's Wi-Fi and pairing: it has no screen to show or confirm the pairing code (the
  service console does it on the bench); leaning: its own setup Wi-Fi offers "Join a master".
- Outputs on a keypad's terminals (a siren wired to a keypad, driven by the master): not built;
  leaning: an IO_SET message beside IO_STATE and a terminal "on keypad n" for wired outputs.
- The RGB LED: a steady colour for the alarm state (never blinking), or off.

## Tasks
- [ ] Phone setup and BOOT recovery on the board, every step (BL-222)
- [ ] The S3 as a keypad of a master, its terminals as that master's zones, on the bench (BL-222)
- [ ] The add-ons on the board: wired zones and outputs, CC1101, DS3231, microSD, ZAC93 (BL-222)
- [ ] The Panel Manager's pages for what only a touchscreen offers today: proposal
  `BL-222-headless-gaps.js` for the owner's approval, then build (BL-222)
- [ ] Outputs on a keypad's terminals (BL-222)
- [ ] An arming automation on the S3 warning on its keypads, Put off from one; a device automation with a Home Assistant or Z-Wave device (BL-222)
- [ ] The board in CI's firmware matrix and on the update server (BL-222)
