# automation_service

**Tier:** services (ESP-IDF, board-agnostic) · **Features:**
[41 Automations](../../../docs/features/41-Automations/README.md) · **Tests:** the store's logic
is host-tested ([automation](../../core/automation/test), `test_auto_store.c`, AUT-008); this
service is the NVS wrapper around it

The panel's built-in automations on the panel: `automation`'s `auto_store` over the NVS namespace
`automations` (`meta`, `a01`..`a16`; DESIGN.md section 6), loaded at start; the readers and edits
the touchscreen and the Panel Manager call; the engine listener; and the `auto` task that runs the
rules (`auto_watch` -> `auto_run`) through `device_service` (DESIGN.md section 5).

## How it runs
- **Listener** (`automation_service_listener()`, on the alarm task): `on_event`, `on_snapshot` and
  `on_zones_reset` feed `auto_watch`; its occurrences go into a 32-deep queue with a 0 timeout (the
  dropped ones counted), then `xTaskNotifyGive`. It never blocks and takes no lock. The snapshot
  also hands over `config_seq`, the clock (`ALARM_TRBL_CLOCK_NOT_SET`) and whether it is locked
  (`alarm_snapshot_locked`: armed or in alarm) as atomics. panel_app
  registers it beside the other listeners, before `alarm_service_start`, without
  `ESP_ERROR_CHECK`: if it isn't taken (`automation_service_listener_added(false)`) the automations
  stay off with a log line, the store still loads and edits still work. The observer's settle time
  starts at 60 s and becomes the configuration's `power_up_settle_s` when start reads it (on the
  alarm task; a changed value re-baselines the observer).
- **The `auto` task** (priority 4, a 6 KB stack in PSRAM, started by `automation_service_start`):
  sleeps until `min(auto_run_next_deadline, 1 s while any time rule exists, else 60 s)`, the cap
  counted from the start of the wake. Each wake: first the rules after an edit or the
  zones after a configuration change (the rules copied out, then `auto_run_rules` outside the lock,
  with the env `automation_service_env` reads), so an occurrence queued before an edit answered
  runs against the new rules (PL04); then, with the time read after it (that read may have
  waited), the occurrences (`auto_run_occurrence`), the device results (`auto_run_result`),
  Try it now requests (`auto_run_try`), the clock (`auto_clock_step` ->
  `auto_run_time`) and `auto_run_tick`; then each stored rule's status (`auto_run_status`) is copied
  out for the readers. Only this task calls `auto_run_*`.
- **The env** (`automation_service_env`, AUT-002, AUT-015): each zone's type, partition and class,
  the enabled partitions and `opt.auto_less_secure` read on the alarm task through
  `alarm_service_call` (no copy of the whole configuration kept), `arm_supported` true (phase
  C); false when the configuration couldn't be read. The task's env, every save's and the editors'
  (their pickers come from `hc_auto_offer` with its `less_secure`) are all this one.
- **Edits** (AUT-010, AUT-015): a save is checked by `auto_store_save_at` against the env read then (with the wall clock in epoch seconds while
  the latest snapshot says the clock is set, else 0: the record's `changed_at`, D30),
  under the save mutex - never one a caller hands in, so nobody stores a less secure rule while the
  option is off; it answers `FAILED` when the configuration can't be read. Every edit (save,
  delete, delete by slot, turn off or on) answers `AUTO_SAVE_LOCKED` while the latest snapshot is
  locked (checked holding the save mutex, so an edit queued behind another's write is checked
  when its turn comes; before the listener has heard a snapshot, the alarm service's own copy
  answers). Try it now is not refused while locked: automations keep
  running then. The race: this and panel_web's own `409 locked` gate each read a snapshot, which
  trails the engine by up to one publish, so an edit begun just as the panel is armed may still be
  written. Neither gate guards the alarm itself - the engine never reads automations, and a rule
  saved that instant runs exactly as one saved a moment earlier would.
- **Revocation** (PL04): an edit that succeeds marks its automation revoked (`auto_revoke_t`; a
  delete by slot, every one) in the same `s_auto_lock` step that publishes it, holding the send
  lock. The task holds the send lock across each send through the runner's `send_begin` /
  `send_end` ports (a device action, an Off, an arm, a disarm), and a revoked automation's send
  waits, unchanged, until the task has copied the new rules (which empties the set): turned off
  or deleted, it never goes; an unchanged action does. So once an edit has answered, no send of
  the old version starts; one already sent stays sent. A running "Turn it off after" timer still
  sends its Off after a turn-off, as before.
- **Try it now** (`automation_service_try`): one request at a time, with a sequence number and its
  caller's deadline (100 ms). The task drops a request past its deadline, and runs one only after
  claiming it (an atomic swap of the pending number to 0); a caller that times out makes the same
  swap first and is told busy, so its actions never run later. If the task claimed it first, the
  caller waits for that answer; an answer is taken only when its number matches.
- **Ports**: `device_act` -> `device_service_act_auto` (with the installer option, AUT-015);
  `maker_ok` -> the maker's authority and the installer option, read together on the alarm task
  (`alarm_service_call`) before each less secure send: a system master or installer with the
  option on, or nothing is sent (`maker_gone`, `less_secure_off`). The task's env is refreshed
  only at its next wake, so this live read is what stops the first unlock after an
  installer turns the option off; `device_kind` -> `device_service_get` (listed
  only when the source matches); `source_settled` -> `device_service_source_settled` (Home Assistant
  up and its catalog arrived, both 30 s); `failed` -> `alarm_service_platform_event
  (ALARM_EV_AUTOMATION_FAILED, ...)` with the source `{ALARM_SRC_AUTOMATION, uid}` (the alarm task
  logs it). Phase C: `arm` -> `alarm_service_command_as(ARM, {AUTOMATION, uid}, maker,
  ALARM_SVC_WHEN_DISARMED)` (BAD_STATE is "armed already", NOT_READY names its zone), with
  `ALARM_ARM_BYPASS_FAULTED` dropped unless `opt.auto_less_secure` is on now (read on the alarm task
  first: off, it arms without bypassing and fails on an open zone, never "can't run"); `disarm` ->
  the same with DISARM and `ALARM_SVC_WHEN_NOT_DISARMED` (BAD_STATE is "disarmed already").
  `alarm_service_command_as` posts to the alarm task and waits (`alarm_service_call`): the engine
  stays on its task, nothing writes flash from this task's PSRAM stack, no lock of ours is held.
  `warning` -> the warning mailbox (one `auto_warning_t` per automation, PSRAM, under `s_auto_lock`;
  `automation_service_warnings` / `_warning` copy out); `put_off` ->
  `automation_service_emit_put_off`, a logged placeholder until `ALARM_EV_ARMING_PUT_OFF` is wired
  (C2c). Device results come through
  `device_service_set_result_listener`, under the device lock: mapped (`failed`, `not_allowed`,
  `unavailable` -> refused; anything else -> no answer) and queued (16 deep, 0 timeout).
- **Put off** (`automation_service_put_off`, AUT-013): the Try it now path (`task_request`), the
  task calling `auto_run_put_off` with the wake's local time; the caller has checked the code
  (`ALARM_CMD_CODE_CHECK`, ui_port_esp's `auto_put_off`). `AUTOMATION_POFF_BUSY` when not taken.
- **The chime** (DESIGN.md section 3 "Arming" 2): at a warning's start and once a minute while any
  warning is ON (never during a HOUR, NIGHT or DAY hold), `sounder_play_background(ALARM_SOUND_CHIME)`.
  The "nothing else sounds" rule lives in the mixer (`snd_play_background`, components/core/sounder):
  it starts only over silence and is cut the moment any other sound is asked for, so it never plays
  over, interrupts or delays an alarm, fire, CO, entry, exit or trouble sound (D22; the sounder's
  test). A minute's chime that found something playing is skipped. While a NIGHT or DAY hold waits
  (`auto_run_wants_clock`) the task wakes at least once a second, as for a time rule.
- **Local time**: `gettimeofday` + `localtime_r` under the panel's TZ (net_wifi sets it), through
  `auto_local_from_civil`; valid while the snapshot's clock trouble is clear.
- **Console** `automations` (read-only, AUT-010): one line per stored rule - slot, uid, on/off, how
  it loaded (ok/newer/broken/bad), the trigger, each action, the last status (`tried` when it came
  from Try it now) - then the store's gen,
  read-only or not, the listener, the task (with its free stack) and the dropped counts.

## Public API
| Header | What it offers |
|---|---|
| `include/automation_service.h` | `automation_service_listener()` and `_listener_added()`, `automation_service_start()` (loads the store, starts the task), `automation_service_list()`, `automation_service_get()`, `automation_service_gen()`, `automation_service_read_only()`, `automation_service_env()` (the panel as the validator sees it now), the edits `automation_service_save()`, `_delete()`, `_delete_slot()`, `_set_off()` (answers: `auto_save_t`, `AUTO_SAVE_LOCKED` while armed or in alarm), `automation_service_status()`, `automation_service_try()` (`AUTOMATION_TRY_BUSY` when the task hasn't taken it within 100 ms; then it never runs), `automation_service_console_register()` |

## Depends on
- Project components: public `automation`, `platform_esp` (the listener type); private
  `device_service`
- ESP-IDF: private `nvs_flash`, `esp_hw_support`, `console`

## Options
| Kconfig (`alarm_features`) | Default | Effect |
|---|---|---|
| `ALARM_FEATURE_AUTOMATIONS` | y with `SPIRAM` (needs `ALARM_HOME_CONTROL`) | The automations; off, nothing is stored or loaded and every edit is refused `READ_ONLY` |

## Resources
- PSRAM (`EXT_RAM_BSS_ATTR`, about 60 KB, all static): the store (16 rules and the bytes of any
  undecodable record, about 15 KB), the readers' view and the task's copy of the rules (7 KB each),
  the runner (`auto_run_t`, about 24 KB), the zones, the task's 6 KB stack, the occurrence queue's
  storage and the console's one rule. Internal RAM: about 1 KB (the task's TCB, the observer, the
  queues' and locks' control blocks, the 16-deep result queue). No heap after start: the console
  prints one rule at a time.
- NVS: at most 291 entries of the 3,906 usable. An add is refused below
  `AUTO_NVS_ADD_MIN` free entries (the alarm configuration's worst rewrite, one record, and the
  add itself).

## Rules
- Two locks. The save mutex is held across an edit's env read, validate and NVS write (only edits
  take it); `s_auto_lock` only for short copies (the readers' view, the gen, the "rules changed"
  flag, the statuses). Readers take only `s_auto_lock`, so a list never waits behind an NVS write.
  Order: save mutex, then `s_auto_lock`, never the reverse. `s_auto_lock` is never held while
  calling `device_service_*` or `alarm_service_*`; the save mutex is held across a save's
  `alarm_service_call` (the env), which can't deadlock: the alarm task never takes it.
- Every reader copies out: nothing hands out a pointer into the store.
- The `auto` task's stack is in PSRAM: it never writes flash or NVS. Everything it calls only
  queues (`device_service_act_auto` to ha_link) or waits for the alarm task
  (`alarm_service_platform_event`, `alarm_service_call`).
- Edits write NVS on the caller's task, which must have an internal-RAM stack (the LVGL task, the
  HTTPS worker); an edit from a PSRAM-stack task is refused and logged (flash auto-suspend is off).
- A record that can't be decoded is kept as stored and never overwritten; only a delete removes it.
- Off means stubs: callers need no guard (components/services/alarm_features).
