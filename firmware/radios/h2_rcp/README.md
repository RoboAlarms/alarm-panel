# h2_rcp - the ESP32-H2 radio's own firmware

The Elecrow wireless ESP32-H2 module in the CrowPanel's wireless socket is the panel's Zigbee
radio (docs/features/20). It runs this: an IEEE 802.15.4 **radio co-processor (RCP)**, ESP-IDF's
OpenThread RCP (`examples/openthread/ot_rcp`, public domain) with its host link fixed to the
UART that reaches the socket. The panel runs the Zigbee stack itself (the gateway layout
Espressif supports with an ESP32-P4 host) and drives the H2 over that UART with the Spinel
protocol at 460800 baud. The H2 keeps no Zigbee state.

Not a panel board: no board component, no signed alarm image, not checked by
`tools/check_boards.py`.

Its version is its own (`PROJECT_VER` in `CMakeLists.txt`, not git describe): bump it by hand
whenever its software changes, so the Flasher and the panel can tell one build from another.

## Build
```powershell
pwsh tools/idf.ps1 -Project radios/h2_rcp build
```
Output: `firmware/radios/h2_rcp/build/` (`bootloader/bootloader.bin`,
`partition_table/partition-table.bin`, `roboalarms_h2_rcp.bin`), about 260 KB.

## Put it on the module (once, over the module's own USB-C)
The module has its own USB-C port (the H2's USB Serial/JTAG, GPIO26/27) and BOOT/RESET buttons
(its datasheet, section 4.1-4.2). Its first image goes on **before it is plugged into the
panel's socket**, with the module plugged into the PC on its own (not in the panel).

Use the Flasher (docs/features/34): **Advanced tools > Zigbee module**, or the step its guided
setup offers. A firmware bundle carries this software beside the panel's images
(`python tools/flasher/run.py bundle --build <panel build> --radio <this build>`; the bundle
refuses a build from uncommitted source unless given `--allow-dirty-radio`). Then unplug the
module and seat it in the panel's socket with the panel switched off.

### For developers: by hand
1. Plug the module's USB-C into the PC, on its own. A new COM port appears (not the panel's).
2. Flash it (USB Serial/JTAG resets it into its loader by itself; no buttons):
   ```powershell
   pwsh tools/idf.ps1 -Project radios/h2_rcp -p COM<n> flash
   ```
   If it doesn't enter the loader, hold BOOT, press RESET, release BOOT, and try again.
3. Unplug it, then seat it in the panel's socket with the panel switched off.

The panel reaches it over the socket's UART (P4 GPIO53/54 on board V1.1). Later updates from the
panel itself are docs/features/20 step 6: the socket carries no BOOT or RESET line to the H2.

## Which H2 pins the socket uses
The datasheet names the socket pads `U1TXD` / `U1RXD` without their GPIO numbers. This build
leaves UART0 on its own pins - **GPIO24 transmits, GPIO23 receives** on the ESP32-H2 - which is
the expected wiring. If the module's schematic shows other GPIOs behind those pads, set
`CONFIG_RCP_UART_PIN_MANUAL=y` with `CONFIG_RCP_UART_TX_PIN` / `CONFIG_RCP_UART_RX_PIN` in
`sdkconfig.defaults` and build again.

With the module in the socket and the panel running a normal 7" build (Zigbee is in every one), the panel's `zigbee status` says whether the RCP answered and
which version it runs; `zigbee pair` and `zigbee devices` follow (docs/features/20 "Next bench
acceptance").

## What it adds to ESP-IDF's RCP
`main/rcp_pending.c` (docs/bugs/B021), through linker wraps (`main/CMakeLists.txt`): the pending
table's add says when the table is full (the port answered success), so the panel's stack falls
back to Frame Pending on every poll; and a frame to an address still in the table goes out with
Frame Pending set, so a sleepy device polls again for the next frame (Espressif's patch in
espressif/esp-zigbee-sdk#893, promised for the release after esp-zigbee-lib 2.0.4). Check both at
each ESP-IDF update, and drop what ESP-IDF then does itself.

## Rules
- The module's IO12/IO14 share the panel's touch I2C bus: this firmware never drives them.
- The baud rate is the panel's `CONFIG_ALARM_ZB_UART_BAUD` (460800): change both or neither.
- The RCP keeps no Zigbee state; the network lives on the panel.
