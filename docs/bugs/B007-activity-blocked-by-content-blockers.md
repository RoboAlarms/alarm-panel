# B007 · Activity page: "This page could not be loaded · Failed to fetch"

**Status (2026-09-23): fixed, flashed on the local panel (v1 dev, normal backlight) and
checked there through a real browser. Committed and pushed 2026-09-23.** Reported: opening Activity
in the Panel Manager always failed with *This page could not be loaded / Failed to fetch*;
the other pages worked.

## Symptom

Activity showed the error box every time. The Overview's "Recent activity" card silently read
"Nothing recorded yet": it asks for the same data but swallows the error.

## Evidence

- The panel served the endpoint fine. `GET /api/v1/activity` over a pinned TLS connection
  (enrolled-workstation grant, `tools/tests/panel_web_hil.py`'s `Panel`): 200, 3,508 bytes,
  40 ms. Uptime showed no crash or restart around it (the last restart was the logo update).
- A 1:1 loopback relay (one browser connection = one pinned TLS connection, Host/Origin
  rewritten) let the built-in browser (no content blocker) open every page against the real
  panel: Activity loaded. So the request was stopped inside the reporter's browser.
- "Failed to fetch" is how Chrome, Edge and Brave report a request that never got an HTTP
  answer, including one cancelled by an extension (`net::ERR_BLOCKED_BY_CLIENT`).
- **EasyPrivacy line 17501 (2026-09-23 list):** `/api/v1/activity|$~third-party,xmlhttprequest`,
  a rule written for some tracking service's same-site endpoint. It blocks **every** same-site
  fetch/XHR whose URL ends in `/api/v1/activity`, on any site. EasyPrivacy is on by default in
  uBlock Origin, AdGuard and Brave Shields.
- All 49 Panel Manager URLs (every API path, the page and its four assets) were checked with
  `adblockparser` against EasyList, EasyPrivacy, uBlock filters/privacy/quick-fixes/unbreak,
  AdGuard Base and Tracking Protection, and Brave's list (host-anchored `||domain` rules and
  third-party-only rules left out: they cannot match a first-party IP address). Only
  `/api/v1/activity` was blocked.

## Root cause

A content-blocker rule matched the endpoint's name. Nothing was wrong on the panel.

## Fix

- The browser reads the events from **`/api/v1/history`** (checked against the same lists; the
  obvious `event-log` was rejected because EasyPrivacy blocks `/event-log?`). The firmware
  keeps answering `activity` too, for scripts.
- A request that never reaches the panel now says so in words instead of "Failed to fetch":
  check the connection, and if it keeps happening on one page, allow the panel's address in the
  browser's content or tracker blocker (`unreachable()` in `panel.js`).
- `components/services/panel_web/test/panel_pages.test.cjs` fails if an API path matching a known blocker rule
  (`/api/v1/activity`, `/event-log?`, `track`, `analytics`, `beacon`, `telemetry`) comes back.
- The page declares an empty icon (`<link rel="icon" href="data:,">`), so browsers stop asking
  for `/favicon.ico` (a 404, but a TLS request to the single HTTPS worker) on every page.

## Verified

- On the panel (normal v1 dev image, ELF `aeaa1e63…9fdf`): every page of the Panel Manager,
  22 routes including four integration pages and a sensor binding, opened through the relay
  with no error text; Activity lists the events; `history` and `activity` return the same 16.
- PC: `node --test components/services/panel_web/test/panel_pages.test.cjs components/services/panel_web/test/panel_frames.test.cjs` 11/11.

## Not verified

The reporter's own browser and extension were not available: the fix follows from the rule above,
the one mechanism that matches "always, only Activity, Failed to fetch, the API answers".
If Activity still fails there, the page now names the blocker as the likely cause.

## Seen on the way (not changed)

The HTTPS server keeps three connections (`max_open_sockets = 3`, least-recently-used purged).
A browser opens up to six, so a fourth connection closes an idle one; the browser retries on a
new connection (another TLS handshake, 0.4-0.9 s on the panel). Every page still loaded.
Raising the limit costs a TLS session each (PSRAM) and LWIP sockets (budget 24); measure first.
