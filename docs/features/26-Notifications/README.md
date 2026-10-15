# 26 · Notifications: telling a person, not just a system

**Status:** Implemented v1 - the words, the gate and the request builders are host-tested, and
`components/services/net_notify` sends them over HTTPS; ntfy delivered end to end on a panel on
2026-09-21. Left: Pushover against the real service (BL-028), store-and-forward across an outage
on hardware (BL-026) and [B006](../../bugs/B006-notifications-lost-after-outage.md)'s outage
fixes seen on a panel (BL-025) · **Milestone:** M15 · **Depends on:** 01, 05, 07, 09, 14

## Goal
The panel can already tell other *systems* what is happening: Home Assistant over a paired local
link (docs/features/23) and MQTT to any broker (docs/features/10). Neither tells a *person*. Both need a
server already running and configured before an alarm at three in the morning reaches a phone.

This feature is the part every notification service shares: what the panel says about an event,
which events are worth saying anything about, and how not to say too much. The services
themselves are [27 (ntfy)](../27-ntfy/README.md) and
[28 (Pushover)](../28-Pushover/README.md).

A notification is a convenience, never a life-safety path. The siren, the screen and the event
log are what the panel is; this is a message about them. The Emergency screen still never claims
help is coming (docs/features/07 UI-029).

## Reference behaviour
A keypad alarm panel reports to a central station over a phone line, IP or cellular, in
Contact ID or SIA DC-09, and the monitoring service turns that into a push notification. The owner
pays a monthly fee and cannot see the protocol.

**We deviate deliberately.** No legacy alarm protocol and no monitoring company: the panel posts
straight to a notification service the owner chose, over HTTPS, with no account on our side and
nothing in between. That is only honest if the panel is equally clear that nobody is watching -
hence UI-029.

## Requirements

### The words
| ID | Requirement | Verified by |
|---|---|---|
| NOTE-001 | Every `alarm_event_type_t` the engine can log has words. A type added without them must fail the build's tests, not reach a phone as an empty message | `test_notify.c: test_every_event_type_has_words` |
| NOTE-002 | The wording is one copy, shared by the Activity list and every notification service, so the panel and a phone can never describe the same event differently | `test_notify.c: test_golden_wording`; the preview renders identical pixels across the move |
| NOTE-003 | A silent event (duress, a silent panic) is hidden on the keypad but described to a notification service, which decides separately whether to send it (NOTE-038) | `test_notify.c: test_silent_events` |
| NOTE-004 | A trouble says what is wrong and, on the way back, that it is over; a long zone name never pushes " · restored" off the end | `test_notify.c: test_trouble_words` |
| NOTE-005 | The buckets the event log filters by and the ones a notification is switched on by are the same list, in the same order | `test_notify.c: test_categories`, the `_Static_assert` in `ui_text.c` |
| NOTE-006 | A zone card's words ("Closed", "Motion", "Bypassed · Open", "Bypassed · Closed", "Low battery", "Alarm · Smoke detected") and their colour are one copy, `notify_zone_state()`: the touchscreen's Zones view and the Panel Manager's zone list read the same words; bypass retains the device class's active or inactive state | `test_notify.c: test_zone_state_words` |
| NOTE-041 | An event an automation caused never reads as the person who saved it (docs/features/41 D30, AUT-009): "By automation" and which one, by its trigger clause through notify's one hook (`notify_set_hooks`, uid to clause, `changed_at` and maker): "Disarmed" / "By automation · At 10:00 AM on weekdays", "Armed Night" / "By automation · 1 zone bypassed", "Kitchen Window bypassed" / "By automation · <clause>", "Kitchen Window watched again" / "Closed after arming · By automation", "Arming failed" / "Kitchen Window · By automation"; "By a deleted automation" once the uid is gone, "By automation · changed since" for an event older than the automation's last edit, "By automation" alone without a hook or after a restart. A put-off names whose code did it: "Arming put off an hour" / "By Pat · until 11:30 PM", "Night arming skipped tonight" / "By Pat", "Arming skipped today" / "By Pat" | `test_notify.c: test_automation_named_in_words`, `test_automation_gone_changed_or_unknown`, `test_put_off_words`, `test_automation_clause` |

The Z-Wave network's own events (docs/features/36 network.md §15) get their titles the same
way: "Z-Wave device added" / "Z-Wave device added without encryption" (with how it joined),
"Z-Wave device removed", "Z-Wave device left the network", "Z-Wave network reset" and "Z-Wave
radio restarted" - each named after the device's zone when it has one, else its node ("Device
7"). A device's software update (started, updated, failed; docs/features/36 ZW-033) and the
radio's own (network.md §14), a garage door moved through its Z-Wave relay ("Garage door
opening" .. "Garage door didn't move", ZW-035) and a device reporting "Radio interference" have
words too.

### The gate: what actually reaches a phone
| ID | Requirement | Verified by |
|---|---|---|
| NOTE-030 | Ordinary events are rate limited: a burst of 10, then one every 30 s | `test_notify.c: test_bucket` |
| NOTE-031 | A rolling day allows 200 notifications; past that only alarms, to a hard 250, and the integration's page says it has paused | `test_notify.c: test_daily_cap` |
| NOTE-032 | An alarm is never held back by a limit an ordinary evening used up, but two alarms are at least 20 s apart | `test_notify.c: test_alarms_bypass_the_bucket` |
| NOTE-033 | Eight zones tripping during one burglary is **two** notifications: the first at once, then one follow-up a minute later naming three zones and counting the rest. A single-zone alarm gets no follow-up, and a zone already named is not counted twice | `test_notify.c: test_eight_zones_are_two_notifications`, `test_one_zone_has_no_follow_up` |
| NOTE-034 | The same trouble on the same zone is not repeated within an hour, and a restore is sent only when its trouble was | `test_notify.c: test_trouble_collapse`, `test_trouble_restore_needs_its_trouble` |
| NOTE-035 | At most one HTTPS request is in flight, and requests are at least 1.5 s apart | code review; `net_notify` runs one task |
| NOTE-036 | Coming back after an outage never replays the log: alarms go individually up to five, everything else collapses into one "12 things happened while the panel was offline". At most seven notifications, however long the outage; nothing the digest didn't read is written off (a run of records without reports doesn't end it, and an outage longer than one pass reads has the rest replayed) | built (`catch_up()`); HIL |
| NOTE-037 | What the owner switched off costs nothing: it is filtered before any limit is touched. Defaults: alarms, troubles and arming on; zone activity and system events off | `test_notify.c: test_categories_filter_first` |
| NOTE-038 | Silent alarms are sent only when the owner opted in, and the page says why: a push shows on a lock screen somebody else can see | `test_notify.c: test_silent_is_opt_in` |
| NOTE-039 | A clock that steps backwards (SNTP correcting a drifting panel) must not wedge the rate limit | `test_notify.c: test_clock_stepping_back` |
| NOTE-040 | An automation's put-off (docs/features/41 AUT-013) reaches a phone only when it skips an arming ("Night arming skipped tonight", "Arming skipped today"); "Arming put off an hour" stays in Activity, since the ARMED or "Arming failed" that follows says enough | `test_notify.c: test_put_off_push_rule` |

### Delivery
| ID | Requirement | Verified by |
|---|---|---|
| NOTE-010 | An alarm that happens while the Wi-Fi is down reaches the phone when it comes back, and survives a restart in between | built (`net_notify` keeps a delivered position per service in NVS); HIL |
| NOTE-011 | Switching a service on starts from now, never from the panel's first boot: enabling it must not dump the event log to a phone | built (`load_acked()` falls back to the log's newest index); HIL |
| NOTE-012 | Each service keeps its own delivered-position, so one that is failing never holds up another | built (one `proto_report_t` and one NVS key each); HIL |
| NOTE-013 | Every request is HTTPS with a verified certificate, public bundle by default | code review, HIL |
| NOTE-014 | A 4xx is not retried (it will never succeed); a 5xx or a timeout is retried with a growing, jittered gap; a retry is a new request, and an event counts as delivered only once a service took it or refused it for good, each service on its own | `test_delivery.c` (`notify_delivery`) |
| NOTE-015 | Notifications never delay, block or fail the alarm engine: the listener only queues | code review (the listener only sets a task notification) |
| NOTE-016 | Nothing on the screen ever waits for a request: the task holds no lock across a post, publishes a status snapshot for the UI to read, and "Send a test notification" only starts one - its answer is collected by the page's tick | code review; the panel has one LVGL thread |

### On the screen
| ID | Requirement | Verified by |
|---|---|---|
| NOTE-020 | Settings > Integrations is grouped: "Smart home" (Home Assistant, MQTT) and "Notifications" (ntfy, Pushover). The section's subtitle says both | preview (`87_integrations_notifications.png`), docs/features/07 UI-034 |
| NOTE-021 | Setting up a notification service needs the system master's or the installer's code, like every other integration (docs/features/14) | preview (the walk-through enters a code) |
| NOTE-022 | Credentials are write-only on the screen: a stored one shows as "Set", never as its value, and the console never prints one | preview (`NTFY-008`, `PUSH-008` checks) |
| NOTE-023 | Each service has "Send a test notification", which is the only proof that reaches the phone. Its result is worth a toast because the outcome is not on this screen (UI-028) | preview, HIL |
| NOTE-024 | The status line changes at most once a minute and the list never reflows while it does (UI-020, UI-023) | preview, code review |

## Design

```
alarm engine ──on_report──► net_notify (ESP-IDF)        components/core/notify (portable C11)
  (alarm task)                one task, esp_http_client   ├── notify_words.c  the sentences
   queues only,               NVS, TLS via crt_bundle     ├── notify_gate.c   what gets sent
   never blocks                    │                      └── notify_http.c   the requests
                                   ├── ntfy      (docs/features/27)
                                   └── Pushover  (docs/features/28)
```

`components/core/notify` is portable C11 with no heap, no globals and no LVGL, so the awkward parts -
the wording, the flood rules and the request bodies - are all host tests (`test_notify.c`, suite
`notify`) rather than something you discover on a wall at 3 a.m.

**Why the wording moved.** `ui_text.c` already turned an event into a sentence, but it lived in
the UI, and a firmware component must not depend on `alarm_ui`. Rather than keep a second copy
that would drift, the sentences moved into `notify_words.c` and `ui_text.c` became a thin wrapper
that maps a portable accent and icon id back to its own `lv_color_t` and glyph. The move was
proved by rendering all 86 preview screenshots before and after and requiring them to be
byte-identical. This is recorded as **ADR-029**, and the "user-facing strings live in the UI
modules" rule (then in CLAUDE.md, now in CONTRIBUTING.md "Rules for the UI") was amended in the
same change.

**Language.** The sentences are English today. With [Globalization](../37-Globalization/README.md)
(docs/features/37, M22) `notify_words` takes the panel's catalog as an argument, so the Activity
list and the phone read the same entry in the panel's language (I18N-006, I18N-030); the `title[64]`
and `body[96]` buffers become byte budgets every translation must fit (I18N-023).

**Why the panel sends it itself** rather than leaving it to a Home Assistant automation:
**ADR-028**. A notification is most valuable exactly when the house's other computer is off,
updating or unreachable, and the standing requirement is that Home Assistant stays
optional.

**The gate** is pure: it takes an event and the time and answers SEND, HOLD or DROP, with a
follow-up emitted later by `notify_gate_tick()`. All of its state is in the caller-owned
`notify_gate_t`, which is rebuilt on boot - so a reboot loop resets the daily counters, which is
its own trouble and is noted as such.

**After an outage** ([B006](../../bugs/B006-notifications-lost-after-outage.md), 8367725): the
digest is marked delivered only when the service took it (retried after 30 s otherwise), so a
router whose Wi-Fi comes up before its uplink does not write the outage's alarms off; the digest
body stays within three lines; reports the event log could not keep wait while offline instead of
being dropped; the Pushover repeat cancel is owed until the service is reached.

## Interfaces and data

`components/core/notify/include/notify/notify_words.h`
```c
bool notify_words(const alarm_config_t *cfg, const alarm_event_t *ev, bool show_silent,
                  notify_words_t *out);   /* title[64], body[96], cat, accent, icon, silent */
notify_cat_t notify_category(alarm_event_type_t type);
```

`components/core/notify/include/notify/notify_gate.h`
```c
notify_act_t notify_gate_offer(notify_gate_t *g, const alarm_event_t *ev, uint64_t now);
bool notify_gate_tick(notify_gate_t *g, uint64_t now, notify_rollup_t *out);
```

The UI reaches the platform only through `ui_backend_t`
(`components/core/ui_backend/include/ui_backend/ui_backend.h`): `notify_status`,
`notify_test`/`notify_test_result`, `ntfy_get`/`ntfy_set`/`ntfy_new_topic`,
`pushover_get`/`pushover_set`. A NULL `notify_status` hides the whole Notifications group, the
way a NULL `halink_status` hides Home Assistant.

Settings live in each service's own NVS namespace, not in the configuration blob - the blob is
the alarm programming, and this follows the `net_mqtt` precedent. **They are stored in plain
NVS**: this panel has neither flash encryption nor secure boot (see docs/features/15), so a token
here is exactly as exposed as the Wi-Fi password already is. The pages do not imply otherwise.

## Test and acceptance
- Host: `components/core/notify/test/test_notify.c`, suite `notify`: the words for every event
  type, the golden wording, silent events, troubles, the Z-Wave events, the zone card's words,
  the categories, every gate rule, and the ntfy and Pushover request builders with their
  escaping and clean cuts of long names.
- UI: the preview walks Integrations > ntfy > Pushover and fails the build if a group heading is
  missing, if ntfy has no topic ready, if a credential can be read back off the page, if a
  malformed Pushover key is accepted, or if a dead server still looks healthy
  (`87`-`96`).
- By hand: `pwsh tools/ui_sim.ps1` - trip eight zones and see two notifications, not eight; pull
  the network and watch the queue.
- On a panel: only after the simulator, only disarmed, and only when asked.

## Open questions
- **A trouble of its own for a failing notification path.** `ALARM_TRBL_COMM_FAIL` belongs to
  MQTT. A notification service that has been failing for five minutes arguably deserves its own
  `alarm_sys_trouble_t` - but that enum is append-only and stored in the event log, and a panel
  beeping at 2 a.m. because a phone push failed is worse than the thing it reports. For now the
  failure shows on the integration's page and in the console. Decided 2026-09-28: no
  new value - a notification path failing for 15 minutes with something to deliver raises the
  existing `comm_fail` ("Reports not getting through", as MQTT does), cleared when delivery
  resumes (BL-103).
- **"Check the connection"** (docs/features/09 NET-012) still probes only the MQTT broker. A TCP
  connect to `ntfy.sh` would prove almost nothing - the test notification is the real check - so
  extending `net_check_set_service()` to several services is a follow-up, not a prerequisite.
- Whether the digest after an outage should also arrive when the panel restarts, or only when
  the network returns. Today it does both: `catch_up()` runs whenever a service reaches the
  network with its delivered position more than `NOTIFY_DIGEST_MIN` (8) records behind the log,
  start-up included. Decided 2026-09-28: both, as built.

## Tasks
- [x] `components/core/notify` with `notify_words.c`, moved from `ui_text.c` with the pixels proved
- [x] `ui_text.c` refactored to wrap it; CLAUDE.md's strings rule amended
- [x] `notify_gate.c`: the bucket, the daily cap, the alarm rollup, trouble collapse
- [x] `components/core/notify/test/test_notify.c` (suite `notify`), wired into `host/CMakeLists.txt`
- [x] Settings > Integrations grouped, and the section subtitle reworded (UI-034)
- [x] `ui_notify.c`: both pages, the shared category rows, the test action
- [x] The pretend services in `host/ui_preview/host_panel.c` and the walk-through (`87`-`95`)
- [x] `notify_http.c`: the ntfy JSON body and the Pushover form, with their escaping tests
- [x] `panel_log` lost-handler array, so notifications can be a second consumer
- [x] `components/services/net_notify`: the task, NVS, HTTPS, the per-service delivered-position
- [x] The QR code on the ntfy page, behind "Show the QR code" with a 60 s auto-hide
- [x] `host/ui_sim` buttons: ntfy and Pushover set up, services refusing, eight zones at once
- [x] docs/features/21 test-layer row and docs/features/14 access row
- [x] docs/features/15 plaintext-NVS note
- [x] Run ntfy on the panel (2026-09-21): a test, a supervision trouble and a mains-power
      trouble, each read back off ntfy.sh to prove it arrived
- Duplicate of docs/features/28 (Tasks: "Try it against Pushover from the PC, then on the panel",
  BL-028): Pushover on the panel, with the owner's own user key and application
  token
- [ ] Prove store-and-forward on a panel: an alarm raised while the network is down reaches the
      phone when it comes back, and after a restart in between (NOTE-010); a longer outage
      arrives as one digest (NOTE-036); switching a service on sends nothing from before
      (NOTE-011), and one service failing does not hold up the other (NOTE-012) (BL-026)
- [ ] See [B006](../../bugs/B006-notifications-lost-after-outage.md)'s fixes on a panel: with the
      internet blocked at the router and Wi-Fi up, cause a few events (chime toggles, a bypass),
      then unblock - the phone gets the digest once and `events` shows no `report_lost`; with
      MQTT off and notifications on, a six-minute router outage still raises the network
      trouble (BL-025)
