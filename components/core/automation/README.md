# automation

**Tier:** core (portable C11) · **Feature:** [41 Automations](../../../docs/features/41-Automations/README.md) ·
**Tests:** [`test/`](test/) (one suite per `test_*.c`, tagged AUT-NNN)

The panel's built-in automations: one trigger (armed, disarmed, an alarm, a fire or CO alarm, a
zone opening or closing, a time of day) and one or two actions (a device action through the
registry, or - phase C - arming or disarming a partition). What it may do is
`hc_auto_allowed()`'s table (AUT-002, HCTL-011 as lifted by ADR-050 and D23): never a script,
an alarm panel or a siren, never panic or silence, and no bypass of its own (an arming automation may bypass the zones it finds open, D29). Disarm, unlock, open and stop are the
less secure actions: only while the installer option "Allow less secure automations"
(`opt.auto_less_secure`, AUT-015) is on, and after a door, window, motion or alarm trigger only
with that action's confirm (AUT-016). Every save stores the saver as `made_by`; a less secure
action runs only while that slot still holds a system master or installer. The validator checks
on save; a stored pair no automation may do loads BROKEN (the option never does); the runner
re-checks the live kind, the option and the maker before each send. The design is [docs/features/41 DESIGN.md](../../../docs/features/41-Automations/DESIGN.md).

What exists: the model and the validator, the stored record and its store, the observer, the
clock and the runner, with device actions (phase A); arming and disarming actions are validated and
stored but refused (`AUTO_ERR_UNSUPPORTED`) until phase C. The pickers come from home_control's
`hc_auto_offer()`, and both confirms from `auto_action_needs_confirm()` (a scene, a switch's Turn
on or Toggle: D8, D9) and `auto_action_needs_trigger_confirm()` (a less secure action after a door,
window, motion or alarm trigger: D24), the same rules `auto_rule_check()` applies, so the
touchscreen and the Panel Manager offer and confirm exactly what a save accepts.

## Public API
| Header | What it offers |
|---|---|
| `include/automation/automation.h` | the model (`auto_rule_t`, `auto_trigger_t`, `auto_action_t`, the stored enums), the zones that may start one (`auto_zone_usable()`), the less secure actions (`auto_action_less_secure()`), the two confirms (`auto_action_needs_confirm()`, `auto_action_needs_trigger_confirm()`), the validator (`auto_rule_check()`), the stored record (`auto_encode()`, `auto_decode()`, `auto_nvs_entries()`) |
| `include/automation/auto_store.h` | the store over a key-value port (`auto_store_load()`, `_save()`, `_delete()`, `_delete_slot()`, `_set_off()`, `_list()`, `_get()`; answers `auto_save_t`, where `AUTO_SAVE_LOCKED` is the service's) |
| `include/automation/auto_watch.h`, `auto_clock.h` | the engine's events and snapshots as occurrences; the local clock's crossed minutes |
| `include/automation/auto_run.h` | the runner (`auto_run_*`): matching, sending, holding, outcomes (`auto_status_t`, `tried` after Try it now), "Turn it off after", Try it now; the maker and the installer option read live before each less secure send (`maker_ok`); arming and disarming (`src/auto_arm.c`: the warning, the put-off `auto_run_put_off`, the partition gap) once the env says `arm_supported` |

## Depends on
- Project components: `alarm_core` (types, snapshots), `home_control` (device kinds and actions,
  what an automation may do), `rtc_clock` (civil dates)
- ESP-IDF: none (portable)

## Options
None in this component. The panel runs it with `CONFIG_ALARM_FEATURE_AUTOMATIONS`
(`components/services/automation_service`).
