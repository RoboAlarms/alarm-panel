# zwave_zones

**Tier:** core (portable C11) · **Features:** [36 Z-Wave sensors (Zooz ZAC93)](../../../docs/features/36-Z-Wave-Sensors-ZAC93/README.md) ([zones.md](../../../docs/features/36-Z-Wave-Sensors-ZAC93/zones.md)), [22 Zone sources](../../../docs/features/22-Zone-Sources/README.md) ·
**Tests:** [`test/`](test/) (5 suites: `zwave_zones`, `zwz_panel`, `zwz_replay`, `zw_keypad`, `zw_feedback`)

Z-Wave devices as zones: the glue between the panel's Z-Wave network (`zw_net`,
[`zwave_host`](../zwave_host)) and its zone source (`zsrc_zwave`, [`zone_hub`](../zone_hub)), so
the PC simulator and the firmware's `zwave_service` (docs/features/36 Z8) share one instead of
two. `zone_hub` stays free of the Z-Wave stack (a firmware without Z-Wave carries none of it); this
component is the only one that sees both, and nothing depends on it.

The owner of the `zw_net_t` passes its port's news on: `link_up` to `zwz_link_up`, `link_down`
and `restarted` to `zwz_link_down`, `event` to `zwz_event`, `received` to `zwz_received`,
`send_done` to `zwz_send_done` (true for the bridge's own tags, top byte `FE`), `quirk` to
`zwz_quirk`; and it calls `zwz_poll` after `zw_net_poll`, at `zwz_next_deadline` too. In the
firmware the zwave task owns the network and the zone task the hub: the calls are made with the
zone service's lock held.

What it does:
- **Devices.** At each `NETWORK` event (network.md §2's check) the zone source's link goes down
  and up again around the network's home ID and every device of the node cache (ours), none
  (a replaced module: the old bindings match nothing, zones.md §1) or no network at all (another
  system's module: the source stays down). A device is told again when it is added, interviewed
  (`NODE_READY`) or found unusable, and gone when it is removed or reset; reachability is
  `zw_health`'s (`UNREACHABLE`/`REACHABLE`). A check-in of its own that the device confirmed
  (`CHECK_IN`, taken) reaches the zone source alone (`zsrc_zw_check_in`): its zones' grace
  (network.md §8, `T + 10 min`) follows at once, and its inputs are not asked again for it
  (describing the device again would ask them). `zwz_describe()` turns a node record into the zone
  source's facts: the interview's inputs verdict per end point (`zw_iv_inputs`), notification types
  with their event masks, Binary Sensor types, air temperature (a Multilevel Sensor before version
  5 cannot say: offered), Basic, security, check-in, listening, and Alarm Sensor types (from the
  node cache's field 12, BL-085).
- **Reports.** Notification, Binary Sensor, Battery (v2's replace flags and "disconnected"),
  Multilevel Sensor (in hundredths of its unit), Alarm Sensor, Basic Set and Report, Wake Up
  Notification; Hail asks the device for its state; everything else is a frame (the device is
  alive). The end point comes from the Multi Channel wrapper; a command reaching this from a Multi
  Command bundle (`zw_s2`'s job to split it) is handled the same way as any other.
- **Answers.** A command inside a Supervision Get is answered at once with a Supervision Report of
  its session - `FF` when understood, `00` when not - with the request's class and end points
  swapped (command-classes.md §4.14, §1); the questions of command-classes.md §5 are answered with
  `zw_cc_panel_answer` and the identity given to `zwz_init` (its protocol version from the module
  at each link up). A CC that is neither decoded into a report nor answered is counted once per
  node and CC (`stats.unknown`, command-classes.md §2) - repeating it does not count it again.
- **Fresh state** (zones.md §3). The devices the zone source names (`zsrc_zw_take_refresh`) are
  asked: Notification Get per type (version 2 and up), Binary Sensor Get per type (or plain), an
  Alarm Sensor Get per type its Supported Report listed (none when no types are known: every one
  names a type), a Multilevel Sensor Get for air temperature, then Battery Get - at once for a
  listening device,
  at its next wake-up for a sleeping one (`zw_net_send` holds it in the wake-up queue).
- **One at a time.** The bridge's commands to one device go one after the other (answers first,
  then the refresh's Gets), each after the end of the last: `zw_net` queues a sleeping device's
  commands by kind (CC, command, first parameter), and a command to an end point is all Multi
  Channel wrapper to it - two at once would replace each other. The security manager busy:
  again `ZWZ_RETRY_MS` later.
- **Proof of life** (network.md §8). What `zw_health` hears without a command - a listening
  device's ACKs, its pings' too, a command delivered to a sleeping one - reaches the zone source
  as a frame: a quiet listening device checks in through its pings instead of showing CHECK
  after 30 min. A network check sets `zw_health`'s clocks anew, which is no word from anyone:
  they are taken as they are.
- **Quirks** (zones.md §8). `zwz_quirk()` gives the interview a model's row: its lifeline group,
  its Configuration parameters, and `no_supervision` from the row's `no_supervision_before` - the
  node cache keeps no firmware version, so an unknown one counts as below it: that model's Sets
  (the interview's, a check-in's) go without Supervision.

### The panel's Z-Wave (`zwz_panel`, docs/features/36 Z8)

Everything `zwave_service` does that is not the RTOS, so it is proved here: it owns the
`zw_net_t` and the bridge, gives the network its port (the stored keys, inclusion counter,
region, check-ins; the quirks; a device's own check-in), and answers the Z-Wave pages
(`ui_backend_t`'s `zw_*` contract, screens.md §12) the way the PC simulator's backend does
(`host/ui_preview/host_panel.c`, which the UI preview walks through): the radio's standing, the
devices with their zones (the owner's `binding` port reads the hub), a device's page with its
inputs, and the operation running - its steps from the inclusion's and the removals' news, its
countdowns, its result. Requests (`zwz_panel_request`) start or answer an operation; region, reset
and restart end when the radio is up again; a refused one leaves the page's operation as it was.
A device's own check-in (network.md §7) goes to `zw_net_set_check_in` (its refusals as the
pages' `NOT_ALLOWED`, `NO_RADIO`, `INVALID`, `BUSY`); the value it sets is kept (`chk`) and shown
as "changes when it next wakes up" until the device confirms it (`CHECK_IN`) and its record has
it. One the device does not take is dropped - the page shows the interval it kept, nothing to
come, nothing logged. One not confirmed before a restart of the panel lived in `zw_net`'s memory
only: it is set again at the first check that finds the network ours (not when `zw_net` still
carries it).

What the log keeps (network.md §15): a device added (secure, or without encryption and why), one
the installer removed (with the first zone it fed), one that left by itself (reset with its own
button, or no longer listed - not the ones a network reset or a replaced radio forgets), a
network reset (for the user who asked) and a radio that restarted by itself (a watchdog, a
brown-out: serial-api.md §6.8's reasons). What is stored (`zwz_panel_dirty`, `zwz_panel_take_*`):
the keys, the inclusion counter and the node cache, the settings, the devices' check-ins - in the
formats `zwz_panel.h` describes.

### Assigned keypad command adapter (portable, not wired into firmware)

`zw_keypad` validates the assignment's network, generation, node, endpoint and exact
authenticated S2 class, then submits PIN commands to the existing alarm command authority.
It stores results rather than credentials; duplicates (16 recent sequences, 60-second window)
cannot multiply wrong-PIN attempts. A busy port may retry only when nothing executed.
The owner serializes calls and supplies a monotonic millisecond clock. See feature36
ZW-026/027 for pending incoming control, installer assignment and trace privacy integration;
the separate output-feedback path is described below.
`test_zw_keypad` runs commands through the real engine, including refusal, lockout, duress,
partition restrictions, revoked assignments, duplicate/retry expiry and malformed messages.

## Public API
`zw_feedback` is the separate output path: alarm events/snapshots become secure Indicator
commands for explicitly assigned devices. It coalesces state and expires transient refusals,
preserves silent alarm policy and keeps eight persisted destinations bound to their current
network/generation. Firmware listeners feed it; it has no dependency on panel audio synthesis.
`test_zw_feedback` uses the real alarm engine; `test_zwz_panel` also verifies its commands
arrive through real S2 and the simulated ZAC93/FLiRS transport. Physical acceptance is pending.

| Header | What it offers |
|---|---|
| `include/zwave_zones/zw_feedback.h` | `zwf_t`, event/snapshot reduction, assigned output destinations, coalesced secure sending/completion, versioned assignment codec; `zwf_fit()` (whether and why not a device can take feedback), `zwf_slot_of()`, `zwf_slot_for()` |
| `include/zwave_zones/zw_keypad.h` | caller-owned `zwk_t`, verified `zwk_binding_t`, synchronous command port; `zwk_init()` for assignment/revocation, `zwk_receive()` for authenticated notifications |
| `include/zwave_zones/zwave_zones.h` | `zwz_t` (the outbox, the refreshes, the proof of life passed on, counters); `zwz_init()`; from the owner's port `zwz_link_up()`, `zwz_link_down()`, `zwz_event()`, `zwz_received()`, `zwz_send_done()`, `zwz_quirk()`; `zwz_poll()`, `zwz_next_deadline()`; `zwz_is_tag()`, `zwz_describe()` |
| `include/zwave_zones/zwz_panel.h` | `zwz_panel_t`; `zwz_panel_init/load_nodes/start()`; the link's news `zwz_panel_on_control/on_frame/on_text()`, `zwz_panel_poll()`, `zwz_panel_next_deadline()`; `zwz_panel_request()` (also `UI_ZW_REQ_SETUP`: a stopped interview continues), `zwz_panel_interview()`, `zwz_panel_expect_restart()`, `zwz_panel_set_feedback()`; the pages' `zwz_panel_status/devices/device/progress()`, `zwz_panel_generation()`, `zwz_panel_zone_of()`; `zwz_panel_dirty()`, `zwz_panel_take_nodes/keys/cfg/checkins/log()`, `zwz_panel_redo()`; the stored formats `zwz_cfg/keys/checkins_encode/decode()` |

## Depends on
- Project components: `zwave_host` (`zw_net`, the node cache, `zw_iv_inputs`, `zw_health`'s clocks),
  `zwave_proto` (the CC codecs, `zw_cc_panel_answer`), `zone_hub` (`zsrc_zwave`), `ui_backend` (the
  pages' `ui_zw_*` types), `alarm_core` (the event types).
- ESP-IDF: none (portable).

## Options
None. Limits: `ZWZ_OUTBOX` (32 answers waiting), `ZWZ_CMD_MAX` (48 bytes a command), one refresh
per device (`ZSRC_ZW_DEV_MAX`).

## Rules
- Plain C11: no malloc, no globals, no RTOS or ESP-IDF headers; time comes in as `uint64_t` ms;
  warning-free with the portable components' flags.
- The bridge's tags have the top byte `FE`: the owner's own must not (`zw_net`'s are `FF`).
- The owner's own commands to a sleeping device must not be of the bridge's kinds (state Gets,
  Supervision Reports): `zw_net`'s queue would keep only the newer one.

## Tests
Run all host tests with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`; one suite with `ctest --test-dir build/host -R zwave_zones`.

`zwave_zones` (`test_zwave_zones.c`, 21 tests; ZW-004, ZW-010, ZW-012, ZW-013, ZW-014, ZW-015,
ZW-023, ZSRC-007, ZSRC-015): `zw_net` against `zwave_host`'s fake ZAC93 and simulated devices with real
Security 2 (its test sources, unchanged), this bridge, the zone source and the real hub with a
recording port, on a fake clock - the owner here is what the simulator and `zwave_service` are. A
secure door sensor joins and is offered as "Zooz ZSE41 open/close sensor · door contact", its open
and close, tamper and low battery reach its zone, it is trusted for fire, and when it stops
checking in it is unreachable T + 10 min after its last frame and the zone CHECK another T + 10 min
later, to the millisecond; its check-in lengthened from 1 h to 12 h moves its zone's grace only
once it confirms it, then at once and without asking it anything - silent afterwards, it is
unreachable 12 h + 10 min after its last frame and its zone not in CHECK 1 h + 10 min later, only
12 h + 10 min later, to the millisecond; an insecure motion sensor is a zone not trusted for fire; a
two-end-point Access Control device's inputs are zones of their own, asked for their state when
bound, a tamper reaching both, its Supervision Gets answered FF or 00 at its class and end point, a
Version Get answered; the pulled radio is one source trouble after the grace and no zone CHECK, and
when it is back each end point is asked and answers; after a panel restart a sleeper is asked for
its state at its next wake-up, not before, and a module restart is no word from it; a quirk row's
parameter reaches the interview; a removed device's zone shows CHECK at once and the next device
with its node ID never feeds it; a node record as the source's facts; a sleeping device's two end
points asked, and two answers to one end point sent, one after the other; no lifeline, no inputs; a
device without Security 2 shows in the module's log that every watched end point's CCs are asked
(Multi Channel for an end point), its reports decode (typed Binary Sensor, v2 Battery, Multilevel
Sensor at any precision and in Fahrenheit, an idle's parameter, Basic), Hail asks again, a
Supervision Report goes back to its end point and the Version Report carries the module's protocol;
the security manager busy, a Get waits and an answer goes first; a quiet listening device checks in
through its pings for hours; a replaced module's devices are none of the old bindings' (CHECK at
once), another system's module keeps the source down; restarting the radio (the soft reset, the
start-up again) leaves a sleeper's and a mains sensor's zones out of CHECK and raises no trouble
for hours after, both heard again; a quirk row's `no_supervision` reaches the interview (a device
answering every supervised Set "fail" is interviewed without Supervision and ends ready); a v2
Battery "disconnected" reaches the zone like "replace now"; a supervised Notification Report whose
event parameters cannot be interpreted still answers Success, a genuinely undecodable one answers
No Support, and a maximum-length (31-byte) event parameter crashes nothing and the next frame is
still processed; one frame (Multi Command) carrying a Notification and a Battery Report both reach
the zone source; an unknown CC is counted once per node and CC, not again for a repeat; a
broadcast- or multicast-addressed Get is never answered as if singlecast.

`zwz_panel` (`test_zwz_panel.c`, 11 tests; ZW-005, ZW-011, ZW-016, ZW-017, ZW-018, ZW-019, ZW-022): the same
world with the owner `zwave_service` is - it answers the pages' questions through requests, keeps
what is to be stored across a restart and records the log. The first start's keys (stored once,
bound to the module's home ID); a secure door sensor added through the pages (refused outside
installer mode; listening with its countdown, the security question, its PIN, reading, added;
logged, listed, its page with its door contact, its zone); a device without Security 2 (logged
"without encryption"; "Check it now" answered, then not); a removal (its model and zone on the
result, logged with the zone, the binding kept); a device reset with its own button (logged once,
its removal as failed quiet); new devices' check-in and one device's own (sent at its wake-up,
shown until it has it, kept across a restart); one the device answers "fail" (dropped and stored
so, the page back to the interval it kept, nothing logged, its zones' grace unchanged) and the
refusals as the pages hear them (no radio while it restarts, another generation, over 24 h, not
allowed); one chosen but not confirmed when the panel restarts (set again at the first check,
still "to come" on its page, taken at the next wake-up with its zones' grace; once confirmed, a
restart sets nothing); region, restart and reset through the pages (a
refused request keeps the page's operation; the reset logged once, for who asked, with new
keys); a panel restart keeping devices and keys, a watchdog restart logged and a reset-line one
not, a pulled module "not answering"; the stored formats (round trips, a longer later value,
damage refused).

`zwz_replay` (`test_zwz_replay.c`, 23 tests) replays developer-link `zwtrace` recordings
through the same panel state machine, matching outgoing frames, callback IDs, timing and
page state. Fixtures include synthetic startup/noise/restart/windows plus scrubbed physical
ZAC93 SDK 7.18.3 startup and add/remove-window recordings. See `test/traces/README.md` for
how to add recordings and the limits (no paired-device/key-dependent traffic yet).

## Sizes on the P4
RISC-V, `-Os`: 4.6 KB of code for the bridge, 12.6 KB with `zwz_panel`; `zwz_t` 3.4 KB, inside
`zwz_panel_t` (84 KB with the network, static in PSRAM: `zwave_service`).
