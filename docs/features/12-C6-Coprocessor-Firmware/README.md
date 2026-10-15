# 12 · ESP32-C6 co-processor firmware

**Status:** In progress - the C6 as shipped (ESP-Hosted 2.12.3) has carried the panel's Wi-Fi
since 2026-09-19, and the panel's updater for it is built (M6) but has sent no image yet. Left:
our own C6 build with the ESP-NOW bridge, its image checked before it is sent and a failed
transfer proved (BL-048); the link's recovery at runtime (BL-097); recovery documented with photos (BL-072) · **Milestone:**
M2 (use existing firmware), M6 (update path), M7 (ESP-NOW bridge) · **Depends on:** 06, 11, 13

**Code:** `components/services/panel_ota/src/cp_ota.c` (the updater; the `c6` console command
is in `ota_console.c`), `components/services/platform_esp/src/net_wifi.c` (bring-up, version,
the `coprocessor` trouble)

## Goal
Keep the CrowPanel's onboard ESP32-C6 (the P4's Wi-Fi/BLE radio) on firmware we control. Our
build adds an ESP-NOW bridge, so the P4 can use ESP-NOW for the keypad link, and the P4 updates
the C6 automatically.

## Facts (September 2026)
- **Verified on the reference board (V1.1, P4 v1.3, 2026-09-19):** the C6 ships with ESP-Hosted
  co-processor **2.12.3** (Elecrow's current release; no upgrade needed). Our ESP-Hosted 3.0.x
  host (3.0.9 since 2026-10-05) talks to it in compatibility mode ("major version mismatch" is only a hint to update it),
  reads its version (`c6 status`) and brings up Wi-Fi. **The C6 enable (GPIO32) must be configured
  `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_LOW=y` with ESP-Hosted 3.x**: 3.x pulses the pin to the
  "active" level and parks it at the other, and the C6 runs while GPIO32 is high. Elecrow's
  examples say ACTIVE_HIGH because ESP-Hosted 2.x ended its reset sequence at the "active" level.
  With ACTIVE_HIGH on 3.x the C6 stays in reset (`sdmmc_init_ocr ... 0x107`). From 3.0.8
  ESP-Hosted always resets it active-low and the option is gone (docs/features/06, findings 1).
- The factory P4 demo (`elecrow_p4_idf`, IDF 5.5.3) doesn't use the C6 at all.
- The C6 is an **ESP32-C6-MINI-1-N4** (4 MB flash) connected to the P4 over **SDIO** (4-bit on
  board rev V1.1). It runs **ESP-Hosted** co-processor ("slave") firmware. Elecrow's wiki says
  the factory firmware "cannot be directly reprogrammed via P4". Elecrow has since published an
  official upgrade: **C6 firmware V2.12.3 (2026-04-08)** plus a `host_performs_slave_ota`
  project that flashes it **from the P4 over SDIO** (repo folder
  `example/CrowPanel_Advance_ESP32-P4 Display_Firmware_Upgrade_Guide`).
- The factory C6 firmware doesn't support ESP-NOW and, per community reports, doesn't answer the
  version-query RPC; on the reference board it does (2.12.3 was read on 2026-09-19, see the
  first fact above and CP-004).
- **ESP-Hosted 3.0.9** is current (2026-10-05). 3.x MCU hosts are back-compatible with 0.0.6+/1.x/2.x
  co-processors; the recommended order is *upgrade the co-processor first*. Co-processor OTA
  is built in: host-driven over the existing transport (partition, LittleFS or HTTPS image source).
- ESP-Hosted forwards `esp_wifi` calls but **not `esp_now`**. It offers a **peer-data
  custom-RPC channel** (application-defined message IDs, 1-8166 byte payloads) in both
  directions. ESPHome (Sept 2026) implemented ESP-NOW for P4+C6 boards exactly this way,
  with a host shim and a matching co-processor overlay.
- A community project has done C6 OTA from the P4 on this exact board (Elecrow CrowPanel
  ESP32-P4 7.0") to install ESP-NOW-capable firmware.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| CP-001 | M2: Wi-Fi works with the C6 as shipped, or after applying Elecrow's V2.12.3 upgrade | HIL: works with the shipped 2.12.3 (joined, time synced) |
| CP-002 | Our image `crowpanel_c6_cp` = ESP-Hosted 3.x co-processor (SDIO slave pins/width matching the V1.1 host config) + `espnow_bridge` | build + HIL |
| CP-003 | The bridge exposes: init (channel), add/remove peer, send (dst MAC, payload <= 1470), receive (src MAC, RSSI, payload), send status; message IDs and packed structs in one shared header, versioned | host tests of the codec, HIL |
| CP-004 | At boot the P4 reads the C6 firmware version. If it differs from the bundled image, the P4 performs co-processor OTA **only while all partitions are disarmed**, then resets the C6 | built (`components/services/panel_ota/src/cp_ota.c`, OTA-007); version read verified on HIL; transfer not run yet (no newer image) |
| CP-005 | A failed C6 update leaves the previous C6 firmware booting (dual OTA slots on the C6); it is logged and shown as failed on the update page, and raises the `coprocessor` trouble only if the C6 then stops answering (decided 2026-09-28: a C6 still carrying the Wi-Fi is not a trouble) | fault-injection test |
| CP-006 | If the SDIO transport fails at runtime, the P4 resets the C6 (GPIO32), retries, and raises the `coprocessor` trouble after 3 failures | HIL |
| CP-007 | Documented manual recovery: flash the C6 through its UART0 download pins (GPIO9 boot strap) with a USB-UART adapter | doc |

## Design

```mermaid
sequenceDiagram
  participant App as P4 link manager
  participant Host as esp_hosted (P4)
  participant CP as C6: ESP-Hosted + espnow_bridge
  participant Air as ESP-NOW peers
  App->>Host: esp_hosted_send_custom_data(MSG_ESPNOW_SEND, {mac, payload})
  Host->>CP: SDIO RPC (peer data)
  CP->>Air: esp_now_send()
  Air-->>CP: esp_now recv cb (src, rssi, data)
  CP-->>Host: custom data MSG_ESPNOW_RECV
  Host-->>App: registered callback
```

- **Bridge messages** (`components/core/alarm_proto/espnow_bridge_proto.h`, shared by both builds;
  not written yet, BL-048):
  `INIT{version, channel}`, `PEER_ADD{mac, channel}`, `PEER_DEL{mac}`, `SEND{req_id, mac, len, data}`,
  `SEND_DONE{req_id, status}`, `RECV{mac, rssi, len, data}`, `STATS{...}`. Encryption stays in
  our link layer, so no ESP-NOW LMKs are needed.
- **Timeouts**: the host side times out a missing `SEND_DONE` after 200 ms and reports failure,
  so a lost completion never stalls transmit (a known weakness of naive shims).
- **Resync**: after a C6 reset the bridge re-sends `INIT` and re-adds peers from the host's table.
- **Image delivery**: the C6 image is published with each release, stored in the P4's `cp_fw`
  partition (written during the P4's own OTA or by the release package) and verified by SHA-256
  and signature before co-processor OTA ([13](../13-OTA-Updates/README.md)).
- **As built (M6):** `cp_fw` holds a plain C6 app image (today written over USB with parttool);
  the panel parses its header for version and size, `c6 update [force]` or the automatic path
  (once per image version) streams it with `esp_hosted_cp_ota_*`, logs
  `firmware_update/coprocessor_updated` and restarts. The signature/SHA-256 check of the C6 image
  in the P4 is still to come with the release package (M7).

## First-time upgrade path (factory -> ours)
1. M2: done. The factory firmware is already V2.12.3 and works with ESP-Hosted 3.0.7.
2. M7: our `crowpanel_c6_cp` build goes into `cp_fw` and the M6 updater installs it. The shipped
   firmware reports its version, so no "unknown" case is needed on this board.

## Open questions
- Does the V1.1 board route the C6's UART0 and GPIO9 to reachable pads for recovery? (Check the
  V1.1 schematic; to be answered with the recovery documentation, BL-072.)
- BLE on the C6: not needed for v1. Disable to save RAM and airtime? (To settle with our own C6
  build, BL-048.)

## Tasks
- [x] M2: verify the shipped C6 firmware (2.12.3; reset polarity fix above)
- [x] Join a Wi-Fi network on the touchscreen and confirm SNTP (2026-09-19)
- Duplicate of docs/features/06 (Hardware acceptance: "Wi-Fi reconnects after an access point
  reboot", BL-032): was "Reconnect after an access point restart (HIL)"
- [ ] `firmware/crowpanel_c6_cp` build (ESP-Hosted 3.x co-processor, V1.1 SDIO config) (CP-002)
  (BL-048)
- [ ] `espnow_bridge` component + shared protocol header + host tests (CP-003) (BL-048)
- [x] P4-side co-processor OTA manager (version check, disarmed-only, once per version) (M6)
- [ ] Signature / SHA-256 check of the C6 image before sending it (M7, with the release package)
  (BL-048)
- [x] CP-005 decided: a failed transfer with the old image still running is logged and shown
      as failed, not a trouble (`cp_ota.c` run_update); `coprocessor` is for a silent C6 (CP-006)
- [ ] Prove CP-005 with a transfer cut off part-way: the C6 keeps booting its old image and the
  update page says it failed; the first real transfer is docs/features/13's box (OTA-007)
  (BL-048)
- [ ] CP-006: when the SDIO link fails after start-up, reset the C6 (GPIO32), retry, and raise
  the `coprocessor` trouble after 3 failures. Today only the start-up retries every 30 s
  (`net_wifi.c` net_task) (BL-097)
- [ ] Recovery documentation (CP-007): flashing the C6 through its UART0 download pins and the
  GPIO9 boot strap with a USB-UART adapter, with photos of the pads (BL-072)
