# 28 · Pushover: an alert that keeps going until you answer it

**Status:** Implemented v1 - the page, the sender, the emergency repeat with its cancel on
disarm and the key check are built, host-tested and on the panel's firmware. **Not yet run
against Pushover**: it needs the owner's own user key and application token (BL-028). The
low-quota warning and the QR code to the application page are written and host-built, and still to
be seen against the real service (BL-028) ·
**Milestone:** M15 · **Depends on:** 26, 07, 09, 14

## Goal
[Pushover](https://pushover.net) costs a one-off fee per platform and needs two credentials, so
it asks more of the owner than [ntfy](../27-ntfy/README.md) does. It earns that with one thing
ntfy has no equivalent of: **emergency priority**. A priority-2 message is re-sent by Pushover's
own servers, on a schedule the panel chooses, until somebody acknowledges it on their phone. A
notification you slept through is worth very little to a burglar alarm; one that keeps going
until you answer is worth a great deal.

That also makes it the one notification path that can be actively unpleasant, so the panel
cancels the repeat the moment the alarm is dealt with.

## Reference behaviour
The closest analogue is a central station operator ringing the house and then the keyholders in
turn until someone answers - "we keep trying until a human responds". Pushover's emergency
priority is that idea without the monitoring contract. We copy the intent, not the mechanism.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| PUSH-001 | Notifications are sent as a form POST to `https://api.pushover.net/1/messages.json` with `token`, `user`, `message`, `title` and `priority` | `test_notify.c: test_pushover_request`, `test_pushover_escaping` |
| PUSH-002 | Both credentials are the owner's own, entered on the panel: a 30-character user (or group) key and a 30-character application token. Anything else is refused with words that say what is wrong | preview (`95_pushover_bad_key.png`) |
| PUSH-003 | The panel ships **no** application token of its own. Pushover requires each installation of self-hosted software to register its own application, so that each owner gets their own monthly allowance | this document; code review |
| PUSH-004 | Alarms use emergency priority by default: `priority=2`, `retry=60`, `expire=1800`. One toggle turns it off | preview (`94_pushover_page.png`) |
| PUSH-005 | **The repeat is cancelled** when the alarm is disarmed, cancelled or silenced: every emergency message carries a `tag`, and the panel calls `/1/receipts/cancel_by_tag/<tag>.json`. A false alarm must not keep a phone screaming for half an hour | `test_notify.c: test_pushover_cancel` builds the request; built on the panel's side (`net_notify.c`: disarmed, cancelled and silenced mark the partition, `cancel_repeats()` sends the cancel and tries again until Pushover is reached, a 5xx, 429 or no answer included); an alarm still undelivered when its partition was disarmed, cancelled or silenced goes out at high priority, never as an emergency (`test_delivery.c: test_dealt_alarm_is_not_an_emergency`); HIL |
| PUSH-006 | The user key is checked against `/1/users/validate.json` when it is entered, so a typo is caught on the spot rather than at three in the morning | preview (`95_pushover_bad_key.png`, the shape); `validate_user_key()` asks Pushover on the panel; HIL |
| PUSH-007 | Alarms use a chosen sound (default `siren`); everything else uses the account's own | preview |
| PUSH-008 | Neither credential is ever readable: the page shows "Set", the getters return empty strings, and the console prints `set` / `none` | preview (the walk-through fails if a key appears on the page) |
| PUSH-009 | Priority maps to Pushover's scale: alarm 2 (or 1 with the repeat off), trouble 1, arming 0, everything else -1 | `test_notify.c: test_priorities`, `test_pushover_request` |
| PUSH-010 | A 4xx is never retried - Pushover says so explicitly, and a bad token will not become good - and the reason it gave is shown on the page | built (`send_words()` treats a 4xx as final and quotes the answer); HIL |
| PUSH-011 | `X-Limit-App-Remaining` is read from each response; when it runs low the page says so instead of failing quietly at the end of the month | firmware build; the warning shows only against the real service (BL-028): the PC preview's pretend service reports no quota |
| PUSH-012 | At most one request in flight (Pushover allows two connections, and rate-limits above that) | code review; one task |

## Design

**Two credentials, both the owner's.** Pushover's published rule for open-source software that
users install themselves is that each user registers their own application and pastes their own
token, so each gets their own allowance and nobody else's traffic can exhaust it. Baking a
shared token into an open-source alarm panel would burn one 10,000-message allowance across
every install within days, so the panel does not have one. The page says, in one line, where to
get both; the QR code (planned) points at the application-creation page.

**Emergency priority, and its off switch.** The panel sends alarms with:

```
priority=2  retry=60  expire=1800  tag=alarm-<partition>
```

60 s rather than Pushover's minimum 30 because the panel's own siren is already sounding in the
house, and 30 minutes rather than the maximum 3 hours for the same reason. On
`ALARM_EV_DISARMED`, `ALARM_EV_ALARM_CANCELED` or `ALARM_EV_ALARM_SILENCED` the panel calls
`cancel_by_tag` for that partition. Without that cancel the feature would be a liability: a fire
alarm you clear in ten seconds would keep every phone in the house alerting for the full expiry.
This was the single most important correction the design review made.

The default is **on**, by decision, because a burglar alarm you sleep through is the
failure this whole feature exists to prevent. The toggle exists so the next owner of a panel can
choose differently.

**Sounds.** `siren`, `alien`, `persistent`, `echo`, or whatever the account uses. Only alarms
take the chosen one; a trouble at midnight should not sound like a break-in.

## Interfaces and data

NVS namespace `pushover`: `on`, `usr`, `app`, `cats`, `emer`, `retry`, `exp`, `snd`, `dev`,
`silnt`.

`ui_pushover_settings_t` (`ui_backend.h`, in `components/core/ui_backend`) carries
`user_key_set` / `app_token_set` and two independent change flags, so either credential can be
replaced without re-typing the other.

The page, top to bottom: the live status row · **Send notifications** · *Your Pushover account*:
User key, Application token, and a line on where to get them while either is missing · *Alerts*:
Repeat alarms until answered, Sound for alarms · *What to send*: the five shared categories and
the silent-alarms opt-in · *Is it working?*: Send a test notification.

## Test and acceptance
- Host: the form builder in `components/core/notify/test/test_notify.c` - percent-encoding for `&`, `=`, `+` and UTF-8, a
  message truncated at Pushover's 1,024 characters and a title at 250, never mid-character.
- Preview: `94`-`96` - the page, a malformed user key refused, and the service switched on with
  both credentials stored and neither readable.
- By hand: `pwsh tools/ui_sim.ps1`, including the service refusing the keys.
- Against the real service, from the PC first, with a scratch application.
- On the panel, with real keys: send a test, then trip a simulated alarm and confirm the
  emergency repeat starts **and that disarming stops it** - that is the acceptance test that
  matters most here.

## Open questions
- **Targeting one device** (`device=`) rather than every device on the account. The field is
  planned in NVS but has no row yet; most households want every phone.
- Whether a *second* user key (a partner's, or a group key) belongs on this page, or whether a
  Pushover group is the right way to do that. Leaning: a group key already works in the existing
  field, so document it rather than build it.
- Whether to show the remaining monthly allowance as a number on the page, or only warn when it
  is low. Leaning: only warn, so the page stays about what it does rather than what it costs.

## Tasks
- [x] `ui_notify.c`: the page, both credential rows, the emergency toggle, the sound picker
- [x] `ui_pushover_settings_t` and the backend hooks; the pretend service in `host_panel.c`
- [x] Preview walk-through and screenshots `94`-`96`
- [x] `notify_http.c`: the form body and its escaping, with tests
- [x] The emergency fields and `cancel_by_tag`'s request, host-tested (the sending is below)
- [x] Sending it for real in `components/services/net_notify`
- [x] Emergency priority **and `cancel_by_tag` on disarm** (PUSH-005) - the panel watches for
      disarmed, cancelled and silenced and cancels that partition's repeat
- [x] `/1/users/validate.json` when a key is entered (PUSH-006)
- [x] Read `X-Limit-App-Remaining` from each response and warn on the page when it runs low
      (PUSH-011): `net_notify.c` `header_seen`; the status line turns amber with "Only N
      messages left this month" under 1,000
- [x] The QR code to Pushover's application-creation page (`pushover.net/apps/build`), shown on
      request while a credential is missing (`ui_notify.c`)
- [ ] Try it against Pushover from the PC with a scratch application, then on the panel with
      real keys: a test, then a simulated alarm whose repeat stops when it is
      disarmed ("Test and acceptance") (BL-028)
