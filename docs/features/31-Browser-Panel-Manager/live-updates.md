# Live updates over a WebSocket (planned)

Part of [31 · Browser Panel Manager](README.md). **Status:** Planned (2026-09-30, BL-129; asked for:
"The WebSocket can be used all over the WebUI not just the walk-test which will make everything
else feel snappy"). The requirements below join the README's table (WEB-026..031) when they are
built; until then this page is their home.

## Why
Every page that shows something changing asks the panel again on a timer
(`poll()` in `panel.js`):

| Page | What it asks, how often |
|---|---|
| Overview | `status` every 2 s, `zone-status` every 3 s, `history` every 5 s |
| Zones | the same three |
| Troubles | `status` every 2 s, `zone-status` every 3 s |
| Home Assistant pairing, software updates, Z-Wave operations, the Z-Wave radio's update, a language download | their state every 1-5 s while something runs |
| Activity, System health | nothing: the page is as old as its last load ("Refresh the page for new values") |

So a change shows up 1-5 s late; a door opened and closed between two reads never shows on Zones
at all; every read is a request for the panel's single HTTPS worker even when nothing changed;
and a walk test on a phone (docs/features/40) would beep up to a second after the door.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| WEB-026 | **One live socket per open page**, `wss://<panel>/api/v1/live`, as the remote keypad's: the sign-in cookie checked before the upgrade, the page's first message carries the session's CSRF token (a page on another site can't read it, so it can't open the socket as you), nothing is sent before it; signing out, the session expiring, its password changing or its user losing access closes it | planned: `live.test.cjs`, the HIL checks (CSRF and origin) |
| WEB-027 | **The panel says what changed** within 250 ms of the change: the status, zone states, troubles, new activity, a walk test's marks, a running operation's progress (Z-Wave, updates, Home Assistant pairing, language downloads), the system's health, and the session itself (locked while armed, signed out). Each message carries the topic and a sequence number, and only what that person may see (WEB-016), built by the same per-viewer code as the matching `GET` | planned: mock and node tests, the Browser pane, a panel |
| WEB-028 | **Nothing is lost when a message is.** On every (re)connect, and on a gap in the sequence, the page reads the full state once with the `GET`s it uses today, then follows the messages. What must not be missed is kept on the panel (the walk test's marks, the activity's sequence numbers), never only in a message | planned: node tests (dropped and reordered messages) |
| WEB-029 | **Polling stays as the fallback.** Without a live socket (refused, closed, an old browser) a page polls as it does today; with one, it keeps one slow safety read (every 30 s) and no other timer | planned: node tests, the mock with the socket refused |
| WEB-030 | **Bounded on the panel.** The alarm task's listeners only set a topic's changed bit and sequence (they never format, allocate or send); the HTTPS worker builds and sends through `httpd_queue_work()`, as the remote keypad does; changes to a topic are coalesced to at most 4 messages a second (a walk test's marks are latched, so coalescing never loses one); at most 3 live sockets, the oldest closed beyond that; internal RAM measured with 0 and 3 live pages (docs/bugs/B001) before `max_open_sockets` (4) is raised | planned: code review, a panel (memory, `display watch` unaffected) |
| WEB-031 | **Kept alive and let go.** The page pings every 10 s, which also keeps httpd's least-recently-used purge from picking the live socket; the panel drops a page silent for 30 s; a hidden tab keeps its socket but redraws only when shown again; a closed or purged socket is reopened with a backoff (1 s, doubling to 30 s), and the page says "Live updates paused" only after 10 s without one | planned: node tests, a phone locked and unlocked |

## Design
- **WebSocket, not Server-Sent Events.** `esp_http_server` has one worker task: an event stream
  would hold it (or need its async request API and a socket per stream anyway), while a
  WebSocket's sends are queued work on that worker and its socket sits idle between them.
  WebSocket support is already in the build (`CONFIG_HTTPD_WS_SUPPORT=y`,
  `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT=y`) with a proven pattern in `web_screen.c`: the
  pre-handshake session check (`panel_web_ws_session()`), the CSRF check
  (`panel_web_ws_alive()`), `send_all()` for short writes, the close callback that forgets a
  socket before its number is reused. A new `web_live.c` follows it; the remote keypad's socket
  stays its own (video, one viewer).
- **Data where it is small, a nudge where it is big.** `status`, `zones`, `troubles` and
  `walk_test` carry their data (the same JSON as their `GET`, a few hundred bytes to a few KB):
  a phone's read back to the panel waits for the panel's Wi-Fi to wake from power saving, a push
  from the panel doesn't. `activity` carries the new rows when they fit in 2 KB, otherwise
  `{"more": true}` and the page reads `history` again (it returns the newest 32 today; an `after`
  parameter, so only the new rows come, is part of this work). Configuration pages get only a nudge
  (`{"topic": "config", "domain": "zwave"}`) and re-read their domain, so a change made at the
  panel or in another browser shows at once instead of being overwritten on save.
- **Message:** one JSON text frame, `{"v": 1, "seq": 812, "topic": "status", "data": {...}}`;
  `seq` is per socket, so a gap means a missed message (WEB-028). The page's messages: the first
  (`{"csrf": ..., "topics": [...]}`: what this page wants), `{"topics": [...]}` when it changes
  page, and pings.
- **Where changes come from:** `alarm_service` listeners (`on_status`, `on_event`) for status,
  zones (`zone-status` is built from the engine's snapshot), troubles, activity and the walk
  test; `zone_service` for a binding's own news (reachable again, a signal reading, W5 of
  docs/features/40); each operation's own progress callback (Z-Wave,
  update download, Home Assistant pairing, language packs); a once-a-second health sample only
  while a page asks for `health`. Every producer sets a bit; one sender per topic runs on the
  worker.
- **Pages:** `panel.js` gets one `live` object (connect, resync, subscribe by topic, the fallback
  to `poll()`), so each page changes from "poll X every n s" to "on X, redraw" with the poll kept
  as the fallback. The Overview's 1 s countdown stays local (a clock, not a read).
- **Words and display:** a live page still changes words at most once a second where they are
  live readings, and nothing flashes when an update lands (the README's "Nothing animates or
  flashes", UI-020's spirit).
- **PC mock:** `mock-panel.cjs` serves `/api/v1/live` beside `mock-screen.cjs`'s socket, pushing
  the mock's own changes (arming timers, the Z-Wave steps, the walk test's pretend trips), with
  `MOCK_LIVE=0` to try the fallback.

## Test and acceptance
- Node tests (`components/services/panel_web/test/live.test.cjs`): the first-message CSRF rule,
  resync on connect and on a gap, the backoff, the fallback to polling, a topic change on
  navigation; the mock panel's socket against them.
- The Browser pane against the mock: every page that polled today updates on a change without
  its timers.
- On a panel: the latency of a zone's change to a phone and to a computer with Wi-Fi power saving
  on; internal RAM and the HTTPS worker's stack with 3 live pages and the remote keypad open; a
  sign-out and an expired session closing the socket; `display watch` unaffected.

## Tasks
- [ ] `web_live.c`, the topics and their producers, the `live` object in `panel.js`, every polling
  page moved to it with polling as the fallback, the mock and the node tests (BL-129)
- [ ] The README's requirement table and "Interfaces" updated with WEB-026..031 when built
  (BL-129)
- [ ] On a panel: latency, memory, sockets, sign-out (BL-129)
