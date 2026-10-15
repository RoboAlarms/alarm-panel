# notify

**Tier:** core (portable C11) · **Features:** [26 Notifications](../../../docs/features/26-Notifications/README.md), [27 ntfy](../../../docs/features/27-ntfy/README.md), [28 Pushover](../../../docs/features/28-Pushover/README.md) ·
**Tests:** [`test/`](test/) (1 suite: `notify`)

What the panel says about an event, in words, and how much of it reaches a phone. This is the
single copy of the event sentences: the Activity list (`components/ui/alarm_ui/src/ui_text.c`)
and a push notification (`components/services/net_notify`) both read them from here, so the
screen and a phone cannot describe the same event differently (ADR-029). It also holds the gate
that decides whether an event is sent, folded into an alarm already running, or dropped, and the
builders for the request ntfy and Pushover each want. It sends nothing, opens no socket, keeps
no clock and knows no LVGL: colours and glyphs leave as accent and icon ids that the UI maps
back, and `net_notify` posts what the builders produce.

## Public API
| Header | What it offers |
|---|---|
| `include/notify/notify_words.h` | `notify_words()` (title, body, category, accent, icon, silent flag for one event), system and zone trouble words, cause, zone, user, result and authority names, `notify_category()`, `notify_accent_for_level()` |
| `include/notify/notify_gate.h` | `notify_gate_t`, `notify_gate_offer()` (SEND, HOLD or DROP), `notify_gate_admit()`/`notify_gate_sent()`/`notify_gate_deliver()` (admission apart from delivery, S02), `notify_gate_follow_up()`/`notify_gate_tick()` (the one-minute alarm follow-up), `notify_fan_out()` (one event, each service its own delivery), `notify_gate_paused()`; the limits as `NOTIFY_*` constants |
| `include/notify/notify_http.h` | `notify_priority()`, `notify_priority_dealt()` (PUSH-005), `notify_http_outcome()`, `notify_backoff()`, `notify_may_send()` (NOTE-014/035), `notify_build_ntfy()`, `notify_build_pushover()`, `notify_build_pushover_cancel()` into a caller-owned `notify_request_t` |

## Depends on
- Project components: `alarm_core` (event, configuration, trouble and result types)
- ESP-IDF: none (portable)

## Options
None. `CONFIG_ALARM_FEATURE_NOTIFY` (`firmware/crowpanel_p4/main/Kconfig.projbuild`) switches
the sending side (`net_notify`) on or off; this component stays in every build because the
Activity list needs the words.

## Rules
- The event sentences live here and nowhere else (CONTRIBUTING.md, "Rules for the UI"). Change a
  sentence here, not in `ui_text.c`, update `test_golden_wording`, and prove it with the preview.
- Every `alarm_event_type_t` must have words (NOTE-001). An unknown type falls into
  `NOTIFY_CAT_SYSTEM`, which notifications leave off by default.
- `notify_cat_t` is the same list, in the same order, as the event log's filters and the UI's
  `ui_event_kind_t` (a `_Static_assert` in `ui_text.c`, NOTE-005). `notify_icon_t` is append-only.
- The gate is pure: the caller supplies the time (`uint64_t` ms) and does the sending; all state
  is in the caller-owned `notify_gate_t`.
- Portable rules (CONTRIBUTING.md): C11, no heap, no globals, no RTOS or ESP-IDF headers; the host
  build and the test are warning-free with `-Wconversion -Werror` (`STRICT`).

## Tests
Run all with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`;
this suite alone with `ctest --test-dir build/host -R notify`.
- The words: every event type has words, the golden wording, silent events, troubles and
  restores, the categories (NOTE-001..005).
- The gate: a burst of 10 then one per 30 s (NOTE-030), the 200/250 daily caps (NOTE-031),
  alarms bypass the bucket (NOTE-032), eight zones make two notifications (NOTE-033), a trouble
  once an hour (NOTE-034), filtering before any limit (NOTE-037), silent alarms opt-in
  (NOTE-038), a clock stepping back (NOTE-039).
- The requests: ntfy as JSON to the base URL with UTF-8 titles (NTFY-001), priorities
  (NTFY-009, PUSH-009), Pushover form encoding, emergency repeat and cancel (PUSH-001/-004/-005),
  long names cut cleanly.
- Not here: the outage digest (NOTE-036) and request pacing (NOTE-035) are in `net_notify`.
