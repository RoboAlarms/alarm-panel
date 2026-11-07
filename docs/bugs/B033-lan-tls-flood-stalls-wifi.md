# B033 — A flood of TLS connections from the home network stalls the panel's Wi-Fi

**Status:** fixed in code 2026-11-03 (BL-225 branch, uncommitted). The gate is proven on the 10.1"
bench panel. The 7" (the panel that stalled) and the Wi-Fi co-processor watchdog's restart are not
yet tried. The ESP-NOW filter is built into the C6 image but not installed: a co-processor update
needs the owner's explicit word. Requirement SEC-012 (docs/features/15).

## Symptom

Phones in direct mode (docs/features/43) reconnected to the Panel Manager's HTTPS server every few
seconds. Each reconnection was a new TLS session. On the bench 7" the Wi-Fi co-processor (the C6,
ESP-Hosted over SDIO) then stopped answering RPCs, and the panel fell off the network: the
alarm-server link and alarm reporting went with it. A single connection that was kept open was fine.

This is a denial of service on a security device. Anyone on the home network can open TLS
connections to port 443 (and to 6054, the Home Assistant link) without signing in. Anyone in radio
range can send ESP-NOW frames, which the C6 passed to the P4 over the same SDIO link.

## Evidence

- 7", a service-console capture (`stall-repro.log`): at the worst point, internal RAM's largest
  free block was 1472 B and its minimum 2.5 KB. TLS accept failed with out-of-memory. A later stall
  came with healthy RAM, and idle runs were clean.
- 10.1", 2026-11-03, before the fix: 16 PC threads reconnecting for 120 s. The web got slow
  (worst 9.6 s, 107 of 429 requests failed) and internal RAM's minimum fell to 29 KB. The link held:
  this board idles with about 110 KB internal free, far more than the 7".

## Cause

- `esp_https_server` runs the whole TLS handshake inside httpd's `open_fn`, before any hook the
  caller can add. So nothing could refuse a connection before mbedTLS and esp_tls had allocated for
  it. The small allocations (`esp_tls_t`, httpd's session, lwIP's PCBs and pbufs) are internal RAM,
  which is also what ESP-Hosted's SDIO buffers need.
- An error answered before its request body was read (`error()` in panel_web.c) closed the
  connection, so a refused phone came back with a full handshake.
- Nothing watched the co-processor. When its RPCs timed out, the panel sat off the network with no
  trouble raised.
- The C6 passed on every ESP-NOW frame from any sender (`espnow_bridge.c`).

## Fix

- **One TLS gate for every listener** (`platform_esp` `tls_gate.c`, host test `test_tls_gate.c`):
  - The gate decides on the accepted socket, before anything is allocated.
  - Each address gets a burst of 8 handshakes and 2 more a second; a failed handshake costs 6. All
    addresses together get 16 and 5 a second.
  - Nobody gets a handshake while internal RAM is below 32 KB free or 8 KB in one block.
  - Refusals are logged once every 10 s with a count. httpd's own line per closed socket is off.
  - The Panel Manager and the developer tools use it through `tls_httpd.c`, which is esp_https_server's
    session handling with the gate in front. The Home Assistant link calls it after `accept()`.
- **Shorter handshake timeouts**: 2 s on the Panel Manager and developer tools, 4 s on the Home
  Assistant link (it serves one client at a time). A connection that stalls its handshake holds a
  server for less time.
- **TLS session tickets** on the Panel Manager (`CONFIG_ESP_TLS_SERVER_SESSION_TICKETS`): a phone
  that reconnects resumes without the full handshake.
- **A refused request keeps its connection**: a body of up to 4 KB is read and dropped, and the
  connection stays open. Only a larger one closes it.
- **The co-processor's watchdog** (`net_wifi.c`):
  - The net task asks the C6 something every second.
  - Three timed-out answers in a row raise the co-processor trouble (`ALARM_TRBL_COPROCESSOR`), so
    the keypad, the owner and the server see it.
  - After 60 s of silence the panel restarts, which resets the C6. It waits until no sounder is
    going and no entry or exit time is counting. The armed state survives a restart.
- **ESP-NOW on the C6** (`espnow_bridge.c`): frames from peers the P4 added are passed on at up to
  400 a second; frames from anyone else (a keypad being paired, or a stranger) at up to 20 a second.
  A flood can't crowd the known keypads out.
- **Priority**: alarm reporting outranks a flood. The alarm task, the server link and ESP-Hosted
  all run above the HTTPS server's priority 2, and handshakes run on the HTTPS worker.

## Verified

- Host tests: 160 of 160, `tls_gate` included.
- 10.1" bench, `python tools/bench_tls_flood.py -p <PORT> --host <panel> --seconds 120`. It runs 16
  reconnecting clients, 4 that stall their handshake and a prober, from one PC. Result: **PASS**.
  - The link reported `connected` in all 17 readings.
  - Internal RAM's minimum was 46 KB, against 29 KB before the fix.
  - The gate admitted 222 handshakes and closed 4061 before allocating anything.
- 10.1", port 6054: the Home Assistant link was on, with 8 threads of TLS handshakes for 60 s. The
  gate closed them at the same rate, and the link stayed connected.

## The 7" (2026-11-03)

The same bench run on the 7": **FAIL**. The gate admitted only 7 handshakes, yet internal RAM fell to
2.7 KB and the C6 stopped answering, so the drain is the network load itself, not the handshakes.
The 7" idled at 76 KB internal free, 37 KB below the 10.1" (it is the panel link's master, with Home
Assistant, Zigbee and Z-Wave). The watchdog worked for real: the co-processor trouble at 00:10:03
(panel clock) and the restart 56 s later; with the flood still running it stalled again, and after
the flood the panel came back on its own (76 KB, linked).

Changes since, not yet run under a flood (the bench's flood runs wait for the owner's word):
- Refused or failed connections close with a reset (`SO_LINGER` 0): no TIME_WAIT left in lwIP.
- lwIP's buffers come from PSRAM first (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) and the TCP/IP task's
  queue is 16 deep: the 10.1" idles 15 KB higher (144 KB free, boot minimum 97 KB from 64 KB).
- About 15 KB of task-only statics moved to PSRAM (`ALARM_SLOW_BSS`): the 7" idles at 91 KB free
  (from 76 KB). `memory tasks` (dev builds) lists each task's stack, where it is and its low water.
- An address that proved who it is (a Panel Manager session, a paired phone or Home Assistant) skips
  the gate's shared bucket for 10 minutes (F20).

10.1", 2026-11-03, with all of the above: **PASS**. The link was connected in 17 of 17 readings, internal RAM's
minimum was 62 KB, and 228 handshakes were admitted while 4081 were closed before allocating anything. Its C6 runs 3.0.10-ra4
(the ESP-NOW filter), installed with the owner's word; Wi-Fi, the server link and the panel link came back
after the update.

## Left

- The same bench run on the 7", and its C6 update (the bench's permission check held both).
- The watchdog's restart while armed: the restart waits for no sounder and no entry or exit time.
- The ESP-NOW filter: install the C6 image (`c6 update`) on a bench panel, but only on the owner's
  explicit word (AGENTS.md). Then pair a keypad and flood ESP-NOW from a third board.
- A stalled handshake still holds the HTTPS worker for its 2 s timeout. An attacker on the home
  network can keep the Panel Manager slow, but not the alarm, its link or its reporting. Handshakes
  off the worker would be the next step.
