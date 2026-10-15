# B016 · A network update failed while the screen viewer was open

**Status (2026-09-28): fixed and proven on the bench panel.** Found while verifying
[B002](B002-ota-self-test-heartbeat-race.md) on the replacement bench board (P4 v1.3, dev profile);
caused by one of B002's own fixes. The fix is in `components/services/dev_remote/src/dev_remote.c`
(one setting and its comment).

## Symptom
With `panelctl screen` streaming the panel's screen, `panelctl flash` failed every time:

```
Uploading signed app to bench-panel
panelctl: Remote end closed connection without response
```

The panel had received and verified the whole image (`ota: image ... verified in ota_1`) and staged
it ("Ready: ... (verified, not started)"), but the workstation never got the reply, so it never
asked for the install. Without the viewer the same update went through.

## Evidence (bench panel, 2026-09-28)
- 4 of 4 uploads failed with the viewer streaming; 3 of 3 passed without it (upload only, and a
  whole install with its self-test).
- The viewer's local server running but no page pulling frames: the upload passed.
- A second client polling `info` every second through a 37 s upload (it timed out twice and
  reconnected): the upload passed. So a busy second client is not enough; the frame traffic is.
- A diagnostic build logged the end of the upload handler: `upload reply on fd 50: ESP_OK` - the
  reply was written - followed at once by the viewer's queued reconnects on the listener
  (`esp_tls_create_server_session failed, 0x0050`, `httpd_accept_conn: session creation failed`).
- The same diagnostic build with three sockets instead of two: 2 of 2 uploads passed with the
  viewer streaming.

## Root cause
B002 turned on `lru_purge_enable` for the development HTTPS listener (a dropped Wi-Fi link had left
stale sessions) while it kept `max_open_sockets = 2`. ESP-IDF's server handles one request at a
time and updates a session's "last used" counter only when a request finishes. During a 37 s upload
the viewer's frame requests queue up and time out; the viewer reconnects. When the upload's handler
has replied, the server serves the viewer's old socket in the same pass - which makes it the most
recently used - and then accepts the viewer's new connection: with two sockets in use, the purge
closes the least recently used one, the upload's, while its reply is still on the way. The
workstation's TLS read ends without the reply (`RemoteDisconnected`).

## Fix
`cfg.httpd.max_open_sockets = 3`: the viewer's connection, the upload's, and room for the viewer to
reconnect behind a long upload without a purge. The purge stays for stale sessions. The lwIP socket
budget (24) has room; mbedTLS sessions allocate from PSRAM.

## Verification (bench panel, 2026-09-28)
- The fixed image (0.6.8-test, dev) installed over the network **with the viewer streaming the
  keypad throughout**: upload, install, authenticated reconnect and the 60 s self-test all passed;
  `health_stale` 0, 0 late frames, 0 guard trips. This is B002's step 2.
- The same flash from a panel still on the unfixed firmware, viewer open: failed again, as expected;
  with the viewer closed it passed.

## Remains
- The Panel Manager's own listener (`panel_web.c`, `max_open_sockets = 4`, the browser keypad's
  WebSocket and the pages) was not tried with a browser upload while the remote keypad streams; it has
  more room, but the same pattern could apply. Worth one bench run when the Panel Manager's upload is
  next exercised (BL-023).
