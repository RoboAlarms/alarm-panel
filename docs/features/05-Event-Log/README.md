# 05 · Event log

**Status:** Implemented v1 - The flash ring with power-cut recovery and the MQTT replay logic are
host-tested; the ring, the viewer, Activity and CSV export on the console run on a panel, MQTT
replay not yet against a real broker (BL-026). Left: bench checks of the wrap and power cut
(BL-095) and of queue and flash faults (BL-026), export over MQTT (BL-054).
· **Milestone:** M4, M5 · **Depends on:** 01, 06, 09, 10

## Goal
A power-fail-safe local history of everything the panel did. It feeds the event viewer and
serves as the store-and-forward queue for MQTT reporting.

## Reference behaviour
A classic panel keeps a 100-event log (alarms, troubles, bypasses, open/close, system) viewable at
the keypad by the master/installer, with date and time. A touchscreen panel keeps a larger log and shows
it on the screen.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| LOG-001 | Every engine event (`on_event`) is appended with its sequence number | `test_elog.c: test_append_and_read` + on-target |
| LOG-002 | At least 4096 events on the P4 (512 on the ESP32); oldest are overwritten, erases spread evenly | `test_wraps_and_levels_wear`; P4 keeps at least 6080 |
| LOG-003 | A power loss during a write or erase never corrupts older records (CRC per record, torn slots skipped, sequence recovery at boot) | `test_power_cut_at_every_byte`, `test_record_codec` |
| LOG-004 | Each record keeps wall-clock time when known; otherwise the boot number + seconds since boot, shown as "time unknown" | `test_record_codec` |
| LOG-005 | The last event sequence is recovered at boot and handed to the engine | `test_last_seq_counts_events`, `test_wraps_and_levels_wear`; `alarm_service_set_min_event_seq` |
| LOG-006 | Reportable events not yet acknowledged by MQTT are resent after reconnect, in order | `test_elog.c: test_read_after_*`, `test_report.c`, integration |
| LOG-007 | Viewer with filters: alarms, troubles, arming, bypass, system | preview (`45_event_log.png`), `test_sparse_filter_with_scan_limit` |
| LOG-008 | Silent events (duress, silent panic) are visible only to master/installer: Activity hides them until "Show private events" takes a master or installer code (a partition master's shows only their partitions'), forgotten on leaving Activity, at the screen saver and when an arming starts | code review, preview (`activity_private*`) |
| LOG-009 | Export as CSV/JSON (SD card or MQTT request) | CSV over the service console (M4); over MQTT: later |
| LOG-010 | A record is never dropped in silence: reportable events have queue slots of their own, a failed append is tried again, one that still can't be kept is reported live instead ([10](../10-MQTT-and-Home-Assistant/README.md) MQTT-017), and whatever is lost is counted in NVS and holds the `storage` trouble until it is cleared — a later successful write never erases it | `test_failed_append_is_retried`, `test_report.c: test_lost_marker_is_never_replayed`; queue and flash fault injection on the bench; UI preview (`activity_loss*`) |

## Design (as built)

### Component `components/core/event_log` (portable C11)
Same rules as `alarm_core`: no heap, no globals, no RTOS. The flash is reached through
`elog_flash_t` (read / write / erase, NOR semantics), so the same code runs on an ESP-IDF
partition, on a RAM "flash" in the PC preview, and in host tests that cut the power after any
byte written or erased.

```c
int    elog_open(elog_t *log, const elog_flash_t *flash, uint32_t boot);  /* scan + recover */
int    elog_append(elog_t *log, elog_kind_t kind, const alarm_event_t *ev);
size_t elog_read(elog_t *log, uint32_t before, elog_filter_fn filter, void *ctx,
                 elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_before);
size_t elog_read_after(elog_t *log, uint32_t after, elog_filter_fn filter, void *ctx,
                       elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_after);
int    elog_clear(elog_t *log);
```

- **A circular log of 64-byte records**, written sector by sector (4 KB = 64 records). Before
  the head enters a sector that still holds records (the oldest), the sector is erased; a sector
  that is already blank is not erased again (no wear on the first lap).
- **Power cuts**: every record has a CRC-32. A write interrupted mid-record leaves one unreadable
  slot, which is skipped and never rewritten in place; an interrupted erase only affects the
  sector that was being dropped anyway. A write that failed without touching the flash keeps its
  slot and index.
- **Order**: each record has its own index (+1 per record, never reused), independent of the
  engine's sequence numbers, so the log stays ordered even if an engine ever restarts its count.
- **Reading** is newest first, paged with `before` / `next_before`. The start slot is computed
  from the index (skipped slots only push older records further back), so paging is O(page),
  and `scan` bounds how many slots a sparse filter may look at per call.
- **Reading forward** (`elog_read_after`, M5): oldest first after a log index, for MQTT
  replay. It finds the start slot the same way and steps back over skipped slots; when the
  records after the index were overwritten, it starts with the oldest one kept. Only readable
  records count toward `scan`, so every call with records left makes progress.
- **Boot number**: stamped on every record (with the seconds since that boot), so entries from
  before a restart without wall-clock time still sort and read correctly ("before a restart").

### Record layout (external contract, version 1)
| Offset | Field | Notes |
|---|---|---|
| 0 | `'E' 'L'` | magic |
| 2 | u8 version | 1; fields are only ever added in the reserved bytes |
| 3 | u8 kind | 1 event, 2 "reported" marker (the event with this seq became due for remote reporting) |
| 4 | u32 index | log order |
| 8 | u32 seq | engine event number |
| 12 | u64 epoch_ms | wall clock, 0 = unknown |
| 20 | u32 boot | boot counter |
| 24 | u32 uptime_s | seconds since that boot |
| 28 | u8 type, partition, user, source kind | `alarm_event_type_t` values are append-only |
| 32 | u16 zone, source id | |
| 36 | u8 arg, arg2, flags | |
| 39 | reserved (21 bytes, 0) | |
| 60 | u32 CRC-32 of bytes 0..59 | |

### On the P4 (`components/services/platform_esp/src/panel_log.c`)
- Partition `eventlog`, **384 KB** = 6144 records; at least 6080 are always kept.
- Engine listeners queue records (64 deep) and never wait for flash; a writer task (priority 3)
  appends them. Readers take the log's lock. A failing flash raises the `storage` trouble.
- **Nothing is lost quietly (LOG-010).** The last 16 queue slots are kept for "reported"
  markers, so a burst of ordinary events can't push a report out. The writer tries a failed
  append up to five times (20–160 ms apart, twice while the flash is already known to be
  failing) — a rejected write keeps its slot and index, a torn one is skipped, so a retry never
  duplicates a record. A reportable event that still can't be kept is offered to every handler
  registered with `panel_log_add_lost_handler()` (two slots: `net_mqtt` publishes it live
  instead of waiting for a replay that can never come, `net_notify` pushes it to a phone); it
  counts as delivered when any of them takes it. A report is either replayed from the log or
  published live, never both, so the broker never sees it twice. Whatever is lost after that
  is counted in NVS (`log/lost`, `log/lostrep`) and holds the `storage` trouble: the counters
  latch, so a later successful write never erases the evidence that an alarm report went
  missing. `panel_log_loss()` reads them; `panel_log_clear_loss()` (and erasing the log) clears
  them. On the console `status` and `events` print what was lost and `events clear_loss`
  acknowledges it. Blocking the alarm task on flash is not an option — the retries happen on
  the writer task alone.
- **The writer proves it is still running.** `panel_log_passes()` counts completed writer-task
  passes; it turns at least once a second whether or not there is anything to write, so a value
  that stops moving means the log stopped keeping events. That is one of the five heartbeats a
  firmware update has to show before it is confirmed ([13](../13-OTA-Updates/README.md)
  OTA-005).
- The boot counter lives in NVS (`log/boot`) and never goes backwards: after an NVS erase that
  kept the log, it continues from the newest record's boot number.
- A failed `panel_log_start()` leaves nothing behind (no mutex, queue or task) and nothing
  half-published: the log reports itself unusable, raises the `storage` trouble, and can be
  started again.
- Start-up order: `panel_log_start()` → `alarm_service_set_min_event_seq(panel_log_last_seq())`
  → `alarm_service_start()`, so event numbers continue after a power cut even when the stored
  runtime state is older (it is only saved on arming changes).
- `on_report` writes a "reported" marker with the time it became reportable (the abort window
  for burglary). The MQTT client (docs/features/10) publishes these markers in log order and keeps
  the index of the last one the broker acknowledged (`mqtt/acked` in NVS), so reconnects and
  restarts resend what wasn't delivered (`panel_log_read_after`, `panel_log_newest_index`).

### Viewer (`components/ui/alarm_ui/src/ui_app.c`)
Activity is the whole log (BL-131 finding 7): newest first with icon, words, who or which zone
and when ("10:50 PM", "Yesterday, 9:01 PM", "Sep 19, 10:50 PM"), filters (All, Alarms, Troubles,
Arming, Bypass, System), "Show older events" paging and how much it keeps. Silent events show,
marked "Silent", only after "Show private events" takes a master or installer code (LOG-008;
dimmed while armed or in alarm), with LOG-010's latched loss at the top that only the installer
clears. The Panel Manager's Activity page hides silent events too (`/api/v1/history`,
[31](../31-Browser-Panel-Manager/README.md),
[B007](../../bugs/B007-activity-blocked-by-content-blockers.md)).

### Console (`panel_console.c`)
`events [n] [all]` prints the newest entries from flash (`all` adds report markers);
`log_export` prints the whole log as CSV (index, seq, kind, boot, uptime, epoch, local time,
event name, partition, zone, user, source, args, flags).

## Test and acceptance
- Host: `components/core/event_log/test/test_elog.c` (12 cases) on a simulated NOR flash,
  including a power cut after every one of the ~4,300 bytes written or erased while appending
  across a sector boundary (after each cut the forward and backward reads must agree), and a
  refused and a torn write that are simply tried again (LOG-010).
- Host, the MQTT side of LOG-006 and LOG-010: `components/core/alarm_proto/test/test_report.c`.
- On-target (HIL): fill past the wrap, pull the power mid-write, check recovery and order
  (LOG-002, LOG-003; Tasks, BL-095).
- Still needs a bench rig, with docs/features/10 MQTT-017's fault injection (BL-026): burst past
  the 64-deep queue while the writer is held up, fail the flash for a stretch, and check that
  every accepted report either reaches the broker, is pushed to a phone by `net_notify`, or
  shows up in `log/lostrep` with the `storage` trouble standing (LOG-010; see Open questions).

## Open questions
- Keep a copy on the SD card when one is inserted? (The card is behind the case; leaning no.)
- A report the log couldn't keep counts as delivered when any lost handler takes it, so one that
  only `net_notify` took (pushed to a phone while `net_mqtt`'s live queue refused it) neither
  reaches the broker nor shows in `log/lostrep`. Should it still count in `log/lostrep` for the
  broker's sake ([10](../10-MQTT-and-Home-Assistant/README.md) MQTT-017: "reaches the broker or
  leaves a fault behind")?

## Tasks
- [x] Portable log ring with a flash abstraction + host tests (M4)
- [x] ESP-IDF `esp_partition` backend + writer task (M4)
- [x] Viewer screen + Activity from the log (M4)
- [x] CSV export on the console (M4)
- [x] MQTT replay (M5)
- [x] Loss-aware queue and writer: reserved slots, retries, latching loss counters (LOG-010)
- [x] Show the latched loss on the console (`status` and `events` print it) and acknowledge it
      with `events clear_loss`
- [x] The latched loss in Settings > Event log, cleared by the installer (BL-053, 5153415)
- [ ] Export the log over MQTT (LOG-009; CSV on the console exists) (BL-054)
- [ ] On a panel: fill past the wrap and cut the power mid-write, then check recovery and order
      (LOG-002, LOG-003) (BL-095)
- [ ] On a panel: burst past the 64-deep queue while the writer is held up and fail the flash
      for a stretch; check that every accepted report reached the broker, was pushed to a phone
      by `net_notify`, or shows in `log/lostrep` with the `storage` trouble standing (LOG-010,
      with docs/features/10 MQTT-017's fault injection) (BL-026)
