# 29 · Storage and SD card foundation

**Status:** Implemented v1 - an optional FAT card mounts at `/sdcard` without being changed;
About > Storage and the Panel Manager show the card and the internal flash, and a master or
installer can format a card after confirming. Host-tested and previewed; About > Storage on a
panel with the read-only checks passed (2026-09-21). Left: format, file and removal tests on
a disposable card (BL-034). · **Milestone:** M16 · **Depends on:** 06, 07, 13, 14

## Goal and scope
A card is inserted and the aim is a reusable SD storage foundation, card information
and an explicit format option in About, accurate internal-flash information, and a calmer
About layout. Remove the affiliation sentence. Future features may use the
card, but this change does not move configuration, event logs, firmware or saver pictures.
The alarm must operate normally without a card. No card contents may be erased just to test.

## Reference behaviour
This is RoboAlarms' own storage interface; no reference-panel branding or disclaimer belongs
on the About screen.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| SD-001 | Mount a supported FAT card without modifying its contents or formatting on failure; absence and unreadable media do not block alarm startup | code review, HIL |
| SD-002 | Report card state, physical capacity, filesystem, used/free usable space, and errors; use 64-bit byte counts and honest unknown states | host, preview, HIL |
| SD-003 | Formatting requires a system master or installer, disarmed/silent status, and explicit confirmation that all card data is erased; cancel does nothing | host, preview, HIL |
| SD-004 | Bind confirmation to the observed card generation; reject stale requests and changed card identity before writing | host, review, HIL |
| SD-005 | Mount, capacity scans, format and removal run in a storage worker, never the UI/alarm task; status reads hold only a short snapshot lock | review, HIL |
| SD-006 | Safely unmount before removal, support explicit recheck/reinsert, and clear capacity on lost media; serialize future file users with maintenance | host, preview, HIL |
| SD-007 | SD uses slot 0, 1-bit CLK43/CMD44/D0=39; preserve Wi-Fi SDIO slot 1, UART0 and touch pins | firmware builds, HIL |
| STOR-001 | Show detected flash capacity, running image size including signing overhead, app-slot size/headroom, reserved update slot and remaining partition allocations; never call reserved bytes file space | host, preview, HIL |
| UI-035 | About overview links to Software update, Storage and System details; keep attribution in details, remove the affiliation disclaimer, and preserve update/access behaviour | preview |

## Design and decisions
- Add a portable storage snapshot/policy and an ESP storage worker with bounded command queue.
  Mount at `/sdcard`. Future callers must acquire/release the service's file-access lease and
  close all files before release; maintenance refuses active leases. No public raw card handle.
- Use ESP-IDF 6.1 FatFs lower-level registration/mount APIs so an initialized card with no
  supported filesystem can be reported and explicitly formatted. FAT12/16/32 initially;
  exFAT is not supported by the bundled configuration. Explain this on the page. Format creates
  one FAT volume with two allocation tables and auto-sized clusters; it is not secure erase.
- Keep SDMMC slot 0 alive after initialization; never deinitialize the whole shared controller.
  Initialize the controller/slot before starting the Wi-Fi task to avoid simultaneous host init.
  No GPIO38 power toggle: that pin is this board's UART RX despite another vendor example.
- There is no verified card-detect switch. Poll card status while mounted; failed probing says
  "No readable card", not a false claim that the socket is empty. Check card explicitly after
  insertion or safe removal. A probe changes the generation, invalidating old confirmations.
- Formatting re-probes identity immediately before destruction, checks authority/state again
  in the worker, and runs independently if an alarm starts later. This optional card holds no
  alarm-critical data; formatting must never prevent arming or delay alarm processing.
- Flash measurements come from chip/partition/image metadata, cached outside the UI task.
  Firmware headroom belongs to its fixed slot; the second slot is reserved for updates.
  Reserved data partitions are allocations, not measured filesystem usage. RAM belongs in
  System details. The internal partition table and signing key stay unchanged.
- About uses existing static rows and subpages. Poll live storage labels at most once per
  second without rebuilding a list every tick or adding animation.

## Test and acceptance
Run host policy/accounting tests, full host suite, generated checks, UI preview and simulator
build, and both P4 firmware builds. Preview covers normal/large/full card, unknown/error,
format cancel/confirm/in-progress/failure, stale identity, safe removal, authorization and
navigation to the relocated update UI. Inspect the PNGs. The tests:
`components/core/storage/test/test_storage.c` (SD-001..004, SD-006, STOR-001) and the storage
walk in `host/ui_preview/main.c` (SD-001..006, UI-035; shots `storage_overview` to
`system_details`).

Flash one normal image only when told to, with no dark build first and no display check
before or after (AGENTS.md "Working with a real panel"); the dark build and the console's
`display` checks are optional tools for a suspected refresh problem (AGENTS.md "Display
safety"). Hardware acceptance requires an explicit go-ahead to flash this feature.
Read-only card probing/capacity and Wi-Fi coexistence come first (passed 2026-09-21, below).
Actual format acceptance requires explicit permission to erase a disposable card; implementing a
button does not authorize erasing the inserted card. Then verify mount, file write/read, safe
removal, reinsertion, full/unsupported media and power interruption on disposable media
(BL-034).

**Read-only acceptance on a panel (2026-09-21, an inserted 32 GB FAT32 card, nothing written or
formatted):** the card mounted (capacity 31,914,983,424 bytes, usable 31,897,681,920, free
29,447,880,704, unchanged afterwards) with Wi-Fi and time working. Display stress writing 189
internal-flash sectors in 20 s gave 55.9 fps, zero late frames, zero guard trips and zero touch
I2C errors; heap integrity and the crypto check passed. Flash reports 16,777,216 bytes, the
firmware 2,494,464 bytes (matching the signed binary), the app slot and the update slot 4,718,592
bytes each, so 2,224,128 bytes of growth room.

## Sources
- [ESP-IDF FatFs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/fatfs.html)
- [ESP32-P4 SDMMC](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32p4/api-reference/peripherals/sdmmc_host.html)
- Feature 06 board mapping and installed IDF 6.1 driver source (slot-specific teardown).

## Implementation and validation
`components/core/storage` contains the portable status contract, format policy, binary-size units
and signed-image accounting. `components/services/panel_storage/src/panel_storage.c` owns
SDMMC/FatFs and cached flash metadata (and, with docs/features/33 (1633c0b), an SPI
card on the classic ESP32 boards, from the board's `board_info()->sd`); `panel_storage.h`
documents the file lease API for future consumers. A read-only `storage` console prints exact
byte counts. The UI lives in `ui_storage.c` and `ui_settings.c`. The Panel Manager's Storage
page (docs/features/31, 0e5a1ab) reads the same snapshots and sends
the same check, eject and format requests; a format there needs "ERASE SD CARD" typed
(`components/services/panel_web/src/web_extra.c`).

Format, file integrity and removal/power-loss tests remain unrun (BL-034). The SD slot has no
verified detect or write-protect switch; polling detects lack of response, and formatting checks
CID again before IO. Removing a card during IO can still corrupt it; this is why safe removal is
explicit.

### Planned consumer: annunciation audio

[Feature 08's storage plan](../08-Audio-and-Annunciation/audio-storage.md) (b856211) proposes recorded
word packs, durable user imports and TTS files prepared ahead of use. The supplied library
has a fixed flash budget; all custom audio, TTS models/language data and generated output
require SD with no flash spillover. Recorded/imported playback needs no TTS model; generated-
cache cleanup must preserve those assets. Card failure leaves built-in speech and tones
available. No audio file consumer exists yet (nothing outside `panel_storage` calls
`panel_storage_acquire()`). The current lease is one nonblocking exclusive
mutex, also used by maintenance and card polling; it must not be held through playback or a
resident model's lifetime. The first audio consumer needs
generation-aware reads and quiesce/cancel coordination for safe removal, with card-swap and
IO-failure tests (with the voice work, BL-044). Existing file-integrity and disposable-media
acceptance above remain open (BL-034).
Internal LittleFS assets are separate new work; the current service reports that reservation
but does not mount it. Configuration/event history stay internal as before.

FAT12/16/32 are supported, not exFAT. Unsupported sector geometry is rejected without writing.

## Tasks
- [x] Record scope, requirements, access policy and destructive-operation boundary
- [x] Implement storage service, flash accounting and portable policy tests (63f4dcd)
- [x] Reorganize About and implement card actions with simulated backend
- [x] Run host/UI/simulator/generated/firmware checks and update screenshots
- [x] Authorized dark-build and read-only inserted-card hardware acceptance (2026-09-21, 63f4dcd)
- [ ] Separately authorized disposable-card format and removal tests, only after an explicit
      go-ahead to erase that card: a format, then mount, a file written and read back, safe
      removal and reinsertion, full and unsupported media, and a power cut during a write
      (BL-034)
