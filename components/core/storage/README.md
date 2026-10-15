# storage

**Tier:** core (portable C11) · **Features:** [29 Storage and SD card](../../../docs/features/29-Storage-and-SD-Card/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `storage`)

The portable part of the SD card and flash information: the status snapshot the screen shows
(`storage_sd_t`, `storage_flash_t`), the policy that decides whether a check, an eject or a
format may run right now, the byte-size wording, and how much of an app slot a signed image
really takes. It exists so the rules that protect a card from an accidental or stale format are
host tests. It touches no hardware: mounting, probing, formatting, flash metadata and the
file-access lease are in `components/services/panel_storage/src/panel_storage.c` (SDMMC and FatFs),
and About > Storage is `components/ui/alarm_ui/src/ui_storage.c`. The card holds no
alarm-critical data; the panel runs normally without one.

## Public API
| Header | What it offers |
|---|---|
| `include/storage.h` | `storage_sd_t` (state, card generation, 64-bit capacity/total/free bytes, name, filesystem, message) and `storage_flash_t` (chip, image, slot and partition figures); `storage_allowed()`, `storage_state_name()`, `storage_size()` (bytes, or KiB to TiB with one decimal), `storage_free()`, `storage_image_bytes()` |

## Depends on
- Project components: none
- ESP-IDF: none (portable)

## Options
None.

## Rules
- A confirmation is for one observed card: `storage_allowed()` compares the generation shown
  when it was confirmed with the current one, and every probe changes it, so a stale request can
  never erase a replacement card (SD-004).
- Format needs an authorised user, an idle panel (the caller's disarmed-and-silent check), no
  active file lease, a known capacity and a Ready or Unsupported card (SD-003). Nothing runs while
  the card is Starting or Working (SD-006).
- Byte counts are 64-bit: cards over 4 GiB and full cards must not wrap (SD-002). A signed image
  takes a 4 KiB-aligned payload plus a 4 KiB signature sector (STOR-001).
- No card contents may be erased just to test (docs/features/29): format acceptance needs an
  explicit go-ahead and a disposable card.
- Portable rules (CONTRIBUTING.md): C11, no heap, no globals, no ESP-IDF headers; the host library
  builds with `-Wall -Wextra -Wpedantic -Wconversion -Werror`.

## Tests
Run all with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`;
this suite alone with `ctest --test-dir build/host -R storage`.
- Unauthorised, busy and leased each refuse a format on their own (SD-003).
- A stale generation or a missing snapshot refuses format and eject (SD-004).
- Format only for Ready or Unsupported media with a known capacity (SD-001/003).
- Eject stays latched until a recheck, and Working cannot be re-entered (SD-006).
- 64-bit free space, size units, signed image size and slot overflow (SD-002, STOR-001).
On hardware, mount, capacity and Wi-Fi coexistence passed; format, removal and power-loss tests
on a disposable card are still to run (docs/features/29).
