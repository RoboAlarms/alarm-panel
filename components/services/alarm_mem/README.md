# alarm_mem

**Tier:** services (ESP-IDF, board-agnostic) · **Features:**
[33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md)
(step 3, ADR-037) · **Tests:** by construction and by building every firmware (see below)

Where the panel's big, slow memory goes. Internal RAM is the alarm's reserve (docs/bugs/B001), so
big buffers, task-only statics and the stacks of tasks that never write flash are "slow":
PSRAM on a chip that has it, with no fallback (a request PSRAM can't meet is refused rather
than draining internal RAM), and internal RAM on a chip without PSRAM, where it is the only
RAM. Services use this instead of naming `MALLOC_CAP_SPIRAM`, so the same service runs on the
P4 and on a classic ESP32 unchanged.

## Public API
| Header | What it offers |
|---|---|
| `include/alarm_mem.h` | `ALARM_SLOW_BSS` (statics), `alarm_mem_slow_malloc/calloc` (no fallback on PSRAM chips), `alarm_mem_prefer_slow_calloc` (PSRAM first, internal if short), `alarm_mem_task_create` (a slow-stack task), `alarm_mem_slow_caps`, `alarm_mem_has_psram` |

## Depends on
- Project components: none
- ESP-IDF: `esp_common` (`EXT_RAM_BSS_ATTR`), `freertos`, `heap`

## Options
None of its own. It follows `CONFIG_SPIRAM` and, for statics,
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`.

## Rules
- Never for memory an ISR or DMA touches (`MALLOC_CAP_INTERNAL` / `MALLOC_CAP_DMA` there).
- Never for the stack of a task that writes flash or NVS: flash auto-suspend is off, so the
  cache is disabled during a write and a PSRAM stack would fault (CONTRIBUTING.md, "Rules for the
  firmware").
- Board components don't use it: a board knows whether it has PSRAM (the CrowPanel's LVGL heap
  asks for PSRAM directly). Only services do (`tools/check_architecture.py` keeps boards from
  depending on services).

## Tests
The policy is a compile-time choice with no logic of its own to test on the PC. On the P4 it
takes exactly the capabilities the call sites asked for before (`MALLOC_CAP_SPIRAM |
MALLOC_CAP_8BIT`, the same fallback where there was one), which the P4 build's unchanged
memory map shows; on the classic ESP32 it is exercised by the E32R40T firmware (docs/features/19).
