# panel_storage

**Tier:** services (ESP-IDF, board-agnostic) · **Features:**
[29 Storage and SD card](../../../docs/features/29-Storage-and-SD-Card/README.md) · **Tests:** the
policy (what may be formatted, stale answers, media states) is host-tested
([storage](../../core/storage/test)); the card is checked on a panel with the console's
`storage` command and Settings > About > Storage

The optional microSD card and the flash accounting of Settings > About > Storage. One worker
task owns mounting, checking, ejecting and formatting; the UI and the Panel Manager only read
snapshots and send requests, which the worker checks again on the alarm task (authority, idle).
Ordinary probing never formats; a format is tied to the card that was seen. Future file users
hold a lease and close their files before releasing it. The SD wiring comes from the app
(`board_info()->sd`): SDMMC 1-bit (the 7") or an SPI bus (the classic ESP32 boards; the
service sets the bus up with DMA, so on a bus shared with the 433 MHz header `panel_app`
starts storage before the receiver).

Split out of `platform_esp` in docs/features/33 step 6, together with the `storage` console command.

## Public API
| Header | What it offers |
|---|---|
| `include/panel_storage.h` | `panel_storage_start(const board_sd_t *)`, status and flash snapshots, requests (check, eject, format), file leases, `panel_storage_console_register()` |

## Depends on
- Project components: public `board_info`, `storage`; private `platform_esp` (alarm task checks), `alarm_mem`
- ESP-IDF: private `console`, `fatfs`, `sdmmc`, `esp_driver_sdmmc`, `esp_driver_sdspi`, `esp_driver_spi`, `spi_flash`, `app_update`, `bootloader_support`, `esp_partition`

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `ALARM_FEATURE_STORAGE` (`alarm_features`) | y | The card and the page; off, the service answers with stubs (status "unsupported", requests refused) and the UI hides the page |
| `PANEL_STORAGE_SDMMC` (`Kconfig`) | y where the chip has SDMMC | link the SDMMC card driver |
| `PANEL_STORAGE_SDSPI` (`Kconfig`) | n | link the SPI card driver (about 10 KB of flash and some IRAM: only boards that need it) |

## Resources
- Task `storage`: 6144 bytes, internal (it writes the card), priority 2.
- On the P4, SDMMC slot 0 stays initialised for the whole boot so a card removal can't tear
  down Wi-Fi's slot 1 (ADR-031, 2026-09-21).

## Rules
- Alarm configuration and the event log never live on the card (docs/features/29).
- A format needs the exact confirmation and the card that was seen; nothing formats on its own.
- Off means stubs: callers need no guard (components/services/alarm_features).
