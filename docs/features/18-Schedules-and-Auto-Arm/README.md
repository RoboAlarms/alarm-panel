# 18 · Schedules and auto-arm

**Status:** Planned - auto-arm (SCH-002 to SCH-005, SCH-008) is built as arming by automation,
docs/features/41 phase C (BL-170, ADR-049): a time-of-day automation with the warning and the
one-time delay. Left here (BL-046): `alarm_sched` for exception dates (SCH-001), auto-disarm
(SCH-006) and scheduled users (SCH-007), with their screens · **Milestone:**
later (after M5) · **Depends on:** 01, 03, 09, 35

## Goal
Time-based behaviour: auto-arm with a warning, auto-disarm, users valid only in time windows,
and time-driven outputs. Time comes from SNTP, the battery-backed clock
([35](../35-Battery-Backed-Clock/README.md)) or a time set by hand, with the configured time zone.

## Reference behaviour
A keypad alarm panel supports auto-arm (with a warning period before arming, the option to
delay it, and force-arming by bypassing faulted zones), auto-disarm, and scheduled or temporary
user codes. Some also offer daily schedules that drive devices and scenes.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| SCH-001 | Weekly schedules (up to 16) with time windows per weekday, plus exception dates | unit |
| SCH-002 | Auto-arm: warning beeps and a screen message for N minutes (default 5) before arming, at the chosen level | unit + HIL |
| SCH-003 | Auto-arm can be delayed once by any user code during the warning (default +60 min) | unit |
| SCH-004 | If the partition isn't ready at auto-arm time: either bypass the faulted zones (option) or fail with a `auto_arm_failed` event | unit |
| SCH-005 | Auto-arm uses source `SCHEDULE` (no code, no auto-stay, exit delay applies) | `CORE-ARM-*` semantics |
| SCH-006 | Auto-disarm (optional, off by default) | unit |
| SCH-007 | Users can be limited to schedules (for example a cleaner, weekdays 09:00-13:00) | unit |
| SCH-008 | Schedules never run while the clock isn't set (`clock_not_set` active) | unit |

## Design
A portable `alarm_sched` module next to `alarm_core`. It gets local time from the platform
(`struct tm` plus a validity flag) and issues ordinary engine commands with source `SCHEDULE`.
Scheduled users are enforced by the platform before calling the engine; a small engine hook
may be added later so the check lives in the core. New events such as `auto_arm_warning`,
`auto_arm_failed` and `schedule_user_denied` extend the catalog without renaming anything.

Already in the engine: the source `ALARM_SRC_SCHEDULE` arms without a code
(`arming.c` `quick_allowed`), never turns into auto-stay (`exit_expired` converts only keypad
arming), is named `schedule` over MQTT (`strings.c`) and reads "By schedule" in Activity
(`notify_words.c`). No host test arms from it yet, and the three new events aren't in
`alarm_event_type_t` yet. Disarming still needs a code from every source (`arming.c`
ALARM_CMD_DISARM), so auto-disarm (SCH-006) needs an engine change.

### Auto-arm is an automation (ADR-049)
An auto-arm is "at a time of day, arm Night": docs/features/41 builds it once, as an automation
whose only action is arming (AUT-011 to AUT-014), with SCH-002's warning, SCH-003's delay by any
code, SCH-004 (failing with a reported ARM_FAILED instead of a new `auto_arm_failed` event, or
bypassing the open zones when the automation says so, 41 D29), SCH-005's semantics (its own source kind, `automation`, rather than
`SCHEDULE`) and SCH-008. Auto-disarm (SCH-006) stays here for now; since ADR-050 an automation
may disarm too ("at a time of day, disarm", behind the installer option), so phase C of 41 can
deliver it the same way.

## Open questions
- Is auto-arm useful for your home, or only scheduled users? (Decides the priority.) Decided:
  both are useful - a schedule that arms when the house is usually empty, with its
  warning and a delay by any code (SCH-003); its priority is BL-046's place in the backlog.

## Tasks
- [ ] `alarm_sched`, portable next to `alarm_core`: weekly schedules with exception dates,
  auto-arm from the `SCHEDULE` source, auto-disarm (needs the engine to accept a code-less
  disarm from `SCHEDULE`, SCH-006), scheduled users, nothing while the clock isn't set; host
  tests for SCH-001 to SCH-008, arming from the schedule source (SCH-005) included (BL-046)
- [ ] Schedule editor screens: the exception dates, auto-disarm, a user's schedule (SCH-001,
  SCH-006, SCH-007) (BL-046)
- The auto-arm warning, its delay and auto-arm's editor are built as docs/features/41 phase C
  (BL-170); its bench is C8 there
