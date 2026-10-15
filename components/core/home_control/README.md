# home_control

**Tier:** core (portable C11) · **Features:** [24 Home control](../../../docs/features/24-Home-Control/README.md), [23 Home Assistant integration](../../../docs/features/23-Home-Assistant-Integration/README.md) ·
**Tests:** [`test/`](test/) (3 suites: `home_control`, `dsrc_zw`, `dsrc_ha`)

The model behind the planned Devices screen: lights, switches, locks, climate, covers, fans,
valves, scenes and scripts, wherever they live. A device source implements
`device_source_ops_t` and reports through a `dsrc_sink_t`; the registry (`hc_t`) merges up to
three sources and 64 devices, says which actions need a user code, and tracks one pending action
per device until its result, a 30 s timeout or a dropped connection closes it. Home Assistant is
the first source (`dsrc_ha`): catalog entries the integration marks `control:true` become
devices, and actions go back as `call` messages matched by request id. The panel's own Z-Wave outputs are the
second (`dsrc_zw`, docs/features/41 phase B): switches, plugs and relays, dimmers and garage doors
become devices keyed `zw_<home>_<node>_<gen>_<ep>`, commands are posted to the Z-Wave task and
answered by request id, and a garage door is closed or opened (an open only by a person with a
code, or an automation with the installer option on), never stopped or set to a position. It never touches the
alarm engine (the platform checks a user code with the engine before it calls `hc_act()`), never
replays a command on its own, and does not draw anything.

## Public API
| Header | What it offers |
|---|---|
| `include/home_control/device_source.h` | the source interface: `dsrc_kind_t`, `dev_kind_t`, `dev_action_t`, `dev_info_t` with its `dev_values_t` (level, setpoint, temperature, modes, what a thermostat is doing, since when, what it can do - `DEV_CAP_*`, `dev_can()`; zero = not known), `dsrc_sink_t`, `device_source_ops_t` |
| `include/home_control/home_control.h` | the registry `hc_t`: `hc_init()`, `hc_register()`, `hc_sink()`, `hc_refresh()`, `hc_tick()`, `hc_list()`, `hc_get()`, `hc_outcome_unknown()` (a failed action's outcome isn't known: the registry's own timeout or drop of an action in flight, or a source's answer that it sent it and couldn't confirm it, never a refusal made before sending - HCTL-012), `hc_needs_code()`, `hc_action_valid()`, `hc_act()`; for automations `hc_auto_allowed()` (what an automation may do, with the installer option), `hc_auto_less_secure()` (unlock, open, stop), `hc_auto_offer()` (the rows the automation pickers show for a kind and a source, each one `hc_auto_allowed()` accepts and the source can carry out) and `hc_act_auto()` (refuses with `HC_ERR_NOT_ALLOWED` by the live kind, returns the request id that `hc_port_t.result` carries) |
| `include/home_control/dsrc_ha.h` | Home Assistant as a source: `dsrc_ha_init()`, `dsrc_ha_ops()`, `dsrc_ha_set_sender()`, the feed (`dsrc_ha_link()`, `dsrc_ha_link_at()` with the panel's clock at a drop, `dsrc_ha_catalog_begin()`, `dsrc_ha_catalog_add()`, `dsrc_ha_state()`, `dsrc_ha_result()` - a call_result by request id, its "timeout" an unknown outcome), `dsrc_ha_kind()`, `dsrc_ha_action_word()` |
| `include/home_control/dsrc_zw.h` | the panel's Z-Wave outputs as a source: `dsrc_zw_init()`, `dsrc_zw_ops()`, `dsrc_zw_set_sender()` (a non-blocking post; the outcome through `dsrc_zw_result()`), the feed (`dsrc_zw_link()`, `dsrc_zw_begin()`, `dsrc_zw_add()`, `dsrc_zw_end()`, `dsrc_zw_state()`), `dsrc_zw_level_for()` |

## Depends on
- Project components: none
- ESP-IDF: none (portable); `idf_component.yml` asks for ESP-IDF 6.1 or later

## Options
None in this component. `CONFIG_ALARM_HOME_CONTROL` (`components/services/platform_esp/Kconfig`,
default y, depends on `SPIRAM` and `ALARM_FEATURE_HA`) compiles the panel's wrapper,
`device_service/src/device_service.c` (HCTL-010).

## Rules
- `dev_kind_t` and `dev_action_t` values are stored in events and requests: append, never
  renumber.
- Security-relevant actions need a valid user code, judged by effect: unlock, opening any cover
  (whatever its class) or a water valve, and any cover Position target but exactly 0 - whatever
  the cover's reported position or the direction of travel (HCTL-005; BL-179's question 3, kept
  2026-10-07). Lock, Close, Stop and Position 0 need none. `hc_needs_code()` says which; the
  engine checks the code, lockout included.
- A pending action lives at most `HC_ACT_TIMEOUT_MS` (30 s), or its device's longer `act_ms` (a
  Z-Wave garage door's whole move, its travel time included), and is tied to its source's
  connection generation; nothing is retried automatically (HCTL-012). Every sent action hears
  back once through `port.result` with its request id: the answer, "timeout", or "offline" (its
  connection dropped, or a refresh dropped its device).
- An automation acts only through `hc_act_auto()`, within `hc_auto_allowed()`'s table
  (HCTL-011 as lifted by ADR-050, docs/features/41 AUT-002): lights and switches on, off and
  toggle, fans on and off, lock and unlock, covers closed, opened and stopped, valves closed and
  opened, a thermostat off, a scene run; never a level, a temperature or a script. Unlock, open
  and stop (`hc_auto_less_secure()`) pass only with `less_secure`, the installer option
  (AUT-015). The pickers on the touchscreen and in the Panel Manager are built from
  `hc_auto_offer()` alone, so they offer exactly that table, less what the device's source
  can't do (Z-Wave: no toggle, no garage stop). `hc_result_t` is append-only.
- `dsrc_zw` posts a garage door's close as 0 and its open as 100; the output layer presses only
  when the tilt sensor says the door is the other way, never blind - which is what makes an open
  as safe as a close, so the platform's sender must move the door through
  `zwz_panel_garage_move()` (`ZWO_ALREADY`, `ZWO_ERR_BUSY`, `ZWO_ERR_POSITION`). Stop and a position are
  refused (a press can't do them), and toggle on anything. The platform registers it before Home
  Assistant, so `HC_MAX_DEVICES` can't crowd the panel's own outputs out.
- Calls come on the platform's device lock. An `hc_t` carries its own staging area, so on the
  panel it belongs in PSRAM, not on a task stack.
- Portable rules (CONTRIBUTING.md): C11, no heap, no globals, no RTOS or ESP-IDF headers; time comes
  in as `uint64_t` ms through `hc_tick()`.

## Tests
Run all with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`;
this suite alone with `ctest --test-dir build/host -R home_control`.
- The registry: registration, one source per kind, refresh merging and dropping devices,
  unusable rows skipped, the sink reaching the port (HCTL-001/002).
- Which actions need a code, including a cover's position - a target below the position the
  cover reports still asks, only 0 doesn't (HCTL-005) - and which actions each kind offers
  (HCTL-004).
- Actions and results: success, failure, refusals, a source that says no (HCTL-006); timeout,
  disconnect and reconnect, a dropped device's action failed with its id, duplicate replies,
  request-id wraparound (HCTL-012); a failure cleared by a newer report and kept by the same one
  (BL-175); an unknown outcome where the registry failed an action in flight (timeout, a
  dropped connection or row) or the source says it sent it unconfirmed (the sink's `unknown`:
  Home Assistant's and Z-Wave's "timeout", through `dsrc_ha_result` and `dsrc_zw_result`), a
  refusal made before sending definite (HCTL-012, BL-171).
- What an automation may do over every kind x action, with the installer option on and off,
  the less secure pairs, and `hc_act_auto()` judging the live kind and returning the id its
  result carries (HCTL-011, AUT-002, AUT-015).
- The Home Assistant source: the domain table, action words, catalog, state and link, the
  `call` it formats, and a round trip through the registry.
- The Z-Wave source (`dsrc_zw`, its own suite): listing and kinds, states, every kind x action
  allowed or refused, the posted request id coming back as a result, offline and unknown keys,
  a new list replacing the old, registration order against a full Home Assistant list, and
  `hc_act_auto()` through it, a garage's open with the option on and off (HCTL-001, HCTL-011,
  AUT-002, AUT-015, HCTL-012).
- What Home Assistant says a device can do, since when and what a thermostat is doing
  (`dsrc_ha`, its own suite): the abilities deciding the slider, kept per kind and while
  unavailable, `since` and `hvac_action` kept, and a link drop listed with the panel's clock
  without touching Home Assistant's own times (HCTL-002, HCTL-004, HCTL-006).
The Devices screen and the unlock/open events (HCTL-007) are not built yet (docs/features/24).
