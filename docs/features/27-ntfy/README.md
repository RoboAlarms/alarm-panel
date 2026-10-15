# 27 · ntfy: an alert on your phone, with no account to make

**Status:** Implemented v1 - the page, the QR code and the sender are built and host-tested; the
panel delivered a test and four real events to ntfy.sh on 2026-09-21, each read back off the
topic. Left: a self-hosted server with its own certificate authority (BL-058: one can be saved,
but nothing uses it yet) and ntfy tried again alongside Pushover (BL-028) ·
**Milestone:** M15 · **Depends on:** 26, 07, 09, 14

## Goal
[ntfy](https://ntfy.sh) is the shortest path from this panel to a phone: an HTTPS POST to a
topic, and anyone subscribed to that topic gets a notification. No account, no API key, no app
token, nothing to register. It is free software, so the owner can also run it themselves and
point the panel at their own server - which is the whole reason it is the first service here.

The panel arrives ready: a topic nobody could guess is already generated, the hosted `ntfy.sh` is
already chosen, and the categories most people want are already on. Switching "Send
notifications" on is the only step.

## Reference behaviour
None - a keypad alarm panel has no equivalent. The nearest thing is a monitoring service's push
notification, which needs an account, a subscription and a dealer. This is the same outcome with
none of that;
see [26](../26-Notifications/README.md) for why the panel does it itself.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| NTFY-001 | Notifications are published with a single HTTPS POST to the base URL, carrying JSON (`topic`, `title`, `message`, `priority`, `tags`) | `test_notify.c: test_ntfy_request`, `test_ntfy_utf8_and_quotes` |
| NTFY-002 | The hosted `ntfy.sh` is the default; a server of your own is one choice away, and both work the same on the phone | preview (`88_ntfy_page.png`, `89_ntfy_server.png`) |
| NTFY-003 | A panel that has never been set up already has an unguessable topic, so nothing has to be typed to start | preview (the walk-through fails if no topic is ready) |
| NTFY-004 | The topic is treated as a credential: it is generated, never defaulted to anything guessable, never derived from the panel's name, and "New topic" rolls it behind a confirmation that says every phone must subscribe again | preview, code review |
| NTFY-005 | A self-hosted address must be `https://` or `http://`, and an empty one refuses to enable the service | preview, `be_ntfy_set` |
| NTFY-006 | A topic is letters, digits, `-` and `_`, up to 64 characters (what ntfy accepts), and the refusal says so | preview |
| NTFY-007 | An access token is optional, for a protected server or a reserved topic, and is sent as `Authorization: Bearer` | `test_notify.c: test_ntfy_request`; HIL |
| NTFY-008 | A stored token is never readable: the page shows "Set", the getter returns an empty string, and the console prints `set` / `none` | preview (the walk-through fails if the token appears on the page) |
| NTFY-009 | Priority maps to ntfy's 1-5: an alarm is 5 (max), a trouble 4, arming 3, everything else 2 | `test_notify.c: test_priorities`, `test_ntfy_request` |
| NTFY-010 | A self-hosted server with a private certificate authority works, with the PEM pasted on the service console, following `net_mqtt ca` | HIL |
| NTFY-011 | The QR code encodes the subscribe URL, is not drawn until "Show the QR code" is tapped, says what it gives away, removes itself after 60 s, and never survives leaving the page | preview (`92_ntfy_qr.png`; the walk-through fails if it is drawn unasked, if it doesn't warn, or if it won't go away), docs/features/07 UI-020 |

## Design

**The JSON publish form, not the header form.** The obvious `POST <base>/<topic>` with an
`X-Title:` header breaks on the panel's own strings: trouble titles read `Front Door · Tamper`
and the silent suffix is ` · Silent`, and U+00B7 in an HTTP header would need RFC 2047 encoding.
Posting to the base URL with a JSON body takes UTF-8 directly, so the escaping problem does not
exist. `notify_http.c` escapes the JSON itself rather than pulling `alarm_proto` into this
component for five fields; the escaping is host-tested against quotes, control characters
and a UTF-8 name cut at a length limit.

```
POST https://ntfy.sh/            Authorization: Bearer tk_...   (only if a token is set)
{"topic":"roboalarms-a6efk9p7","title":"Burglary alarm","message":"Front Door",
 "priority":5,"tags":["rotating_light"]}
```

**Why the topic is the security model.** On ntfy anyone who knows a topic can both read and
publish to it. There is no other credential on the hosted service. So the panel generates
`roboalarms-` plus eight characters from an alphabet with no look-alikes, treats it as a secret
everywhere except the one page that has to show it, and offers a way to roll it.

**Rate limits.** `ntfy.sh` allows a burst of 60 requests and then one every 5 s. The shared gate
(docs/features/26 NOTE-030) is far tighter than that - 10 then one per 30 s - so the panel never
approaches the service's limit even during an alarm.

## Interfaces and data

NVS namespace `ntfy`: `on`, `own`, `base`, `topic`, `tok`, `tls`, `ca`, `cats`, `silnt`.

`ui_ntfy_settings_t` (`ui_backend.h`, in `components/core/ui_backend`) follows the MQTT
password convention exactly: `token` is never read back, `token_set` says one is stored, and
`token_change` on the setter is what replaces it.

The page, top to bottom: the live status row · **Send notifications** · *Where they go*:
Server (a picker: ntfy.sh or your own), Address (only when self-hosted), Topic, Access token,
New topic · the QR code · *What to send*: the five shared categories and the silent-alarms
opt-in · *Is it working?*: Send a test notification.

## Test and acceptance
- Host: the request builder in `components/core/notify/test/test_notify.c` - a title with `·`
  in it, a zone name with a quote, and a body at the length limit, none of which may produce
  invalid JSON.
- Preview: `88`-`93` - the page as it arrives, the server picker, switched on, alarms turned off
  (with its warning), the QR code with its warning, and a server that stopped answering.
- By hand, before any panel: `pwsh tools/ui_sim.ps1`.
- Against the real service, from the PC first: the same JSON the host test asserts, posted with
  `curl` to a scratch topic, to confirm ntfy accepts the exact shape.
- On the panel: subscribe a phone to the topic, then "Send a test notification".

## Open questions
- Whether to offer ntfy's `Actions` (a "Disarm" button in the notification itself). It would
  need an inbound path and a code, so it is deliberately out of v1.
- Whether an alarm should set `X-Priority: 5` *and* a distinct tag so phone-side filters can
  tell an alarm from a trouble without parsing the title. Leaning yes, with the tag list frozen
  as a contract once it ships. Decided in `notify_http.c` `ntfy_tag()` (commit
  `342032e`), as leaned: priority 5 travels in the JSON body (the header form is not used),
  and every category has its own tag - alarm `rotating_light`, trouble `warning`, arming `lock`,
  zone activity `door`, system `information_source` - frozen once shipped (never renamed, only
  added to).
- How a self-hosted server's own certificate authority is loaded and chosen (BL-058). NTFY-010
  names the service console, following `mqtt ca`, but the console has no such command; the
  Panel Manager's ntfy page takes the PEM instead. Is a console command still wanted? And does
  the panel use a saved authority whenever the server is self-hosted, or behind a setting of its
  own, as MQTT's "Encryption" choice does?

## Tasks
- [x] `ui_notify.c`: the page, the server picker, the topic and token rows, "New topic"
- [x] `ui_ntfy_settings_t` and the backend hooks; the pretend service in `host_panel.c`
- [x] Preview walk-through and screenshots `88`-`93`
- [x] `notify_http.c`: the JSON body, with tests for UTF-8, quotes and the length limit
- [x] Sending it for real in `components/services/net_notify`, with the token header
- [x] The QR code (`CONFIG_LV_USE_QRCODE=y`; both variants rebuilt from scratch)
- [x] Tried against `ntfy.sh` from the panel (2026-09-21): a test, a zone supervision trouble and
      a mains-power trouble, all four messages read back with the right title, body, priority and tag

### A self-hosted server with its own certificate authority (BL-058)
- [x] A private authority can be saved: the Panel Manager's ntfy page takes the PEM and stores
      it in NVS `ntfy`/`ca` (`web_extra.c` `certificate-authority` calls `net_notify_set_ca()`) (BL-058)
- [ ] Use the saved authority: nothing chooses `NET_NOTIFY_TLS_OWN_CA` yet
      (`net_notify_ntfy_set()` and both pages carry no TLS field, and `tls` is only read back
      from NVS), so `post()` always verifies with the certificate bundle; and the Panel
      Manager's ntfy page does not say whether one is saved (`web_config.c` sends `ca_loaded`
      for MQTT only)
- [ ] `notify ca` on the service console (the command is `notify`; this box first said
      `ntfy ca`), following `mqtt ca` as NTFY-010 says - or NTFY-010 follows the Panel Manager,
      see "Open questions"
- [ ] A self-hosted server with its own certificate authority tried end to end, with an access
      token (NTFY-007, NTFY-010)

### On the panel again
- [ ] ntfy tried again on the panel with the current firmware, alongside Pushover's first run
      (docs/features/28) (BL-028)
